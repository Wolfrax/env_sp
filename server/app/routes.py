from flask import Blueprint, render_template, request, jsonify
from collections import deque
from datetime import datetime, timedelta, timezone
import json
import os
import re
import threading


DATA_DIR = "data"


def safe_id(value):
    return re.sub(r"[^A-Za-z0-9_-]", "_", str(value))


def make_node_id(node):
    name = safe_id(node.get("name", "unknown"))
    location = safe_id(node.get("location", "unknown"))
    return f"{name}_{location}"


def parse_timestamp(timestamp):
    if timestamp is None:
        return datetime.now(timezone.utc)

    if isinstance(timestamp, datetime):
        return timestamp

    if isinstance(timestamp, str):
        return datetime.fromisoformat(timestamp.replace("Z", "+00:00"))

    raise ValueError(f"Unsupported timestamp: {timestamp!r}")


class TimeWindowStore:
    def __init__(self, file_path, window_days=7):
        self.file_path = file_path
        self.window = timedelta(days=window_days)
        self.node = None
        self.data = deque()
        self.logs = deque(maxlen=50)
        self.lock = threading.Lock()
        self._load()

    def add(self, temp, humidity, pressure, timestamp=None, node=None, logs=None):
        timestamp = parse_timestamp(timestamp)

        entry = {
            "t": float(temp),
            "h": float(humidity),
            "p": float(pressure),
            "ts": timestamp,
        }

        with self.lock:
            if node is not None:
                self.node = node

            if logs:
                self.logs.clear()
                for line in logs[-50:]:
                    self.logs.append(str(line))

            self.data.append(entry)
            self._cleanup(timestamp)
            self._save()

    def get_all(self):
        with self.lock:
            return [
                {
                    "t": d["t"],
                    "h": d["h"],
                    "p": d["p"],
                    "ts": d["ts"].isoformat(),
                }
                for d in self.data
            ]

    def get_node(self):
        with self.lock:
            return self.node

    def __len__(self):
        with self.lock:
            return len(self.data)

    def _cleanup(self, reference_time):
        cutoff = reference_time - self.window

        while self.data and self.data[0]["ts"] < cutoff:
            self.data.popleft()

    def _serialize(self):
        return {
            "node": self.node,
            "logs": list(self.logs),
            "data": [
                {
                    "t": d["t"],
                    "h": d["h"],
                    "p": d["p"],
                    "ts": d["ts"].isoformat(),
                }
                for d in self.data
            ],
        }

    def _deserialize(self, obj):
        self.node = obj.get("node")
        self.logs = deque(obj.get("logs", []), maxlen=50)

        return deque(
            {
                "t": float(d["t"]),
                "h": float(d["h"]),
                "p": float(d["p"]),
                "ts": parse_timestamp(d["ts"]),
            }
            for d in obj.get("data", [])
        )

    def _save(self):
        os.makedirs(os.path.dirname(self.file_path), exist_ok=True)

        tmp_file = self.file_path + ".tmp"

        with open(tmp_file, "w", encoding="utf-8") as f:
            json.dump(self._serialize(), f, indent=2)

        os.replace(tmp_file, self.file_path)

    def _load(self):
        if not self.file_path or not os.path.exists(self.file_path):
            self.data = deque()
            self.node = None
            return

        try:
            with open(self.file_path, "r", encoding="utf-8") as f:
                obj = json.load(f)

            self.data = self._deserialize(obj)

            now = datetime.now(timezone.utc)
            self._cleanup(now)

        except Exception as err:
            print(f"Failed to load {self.file_path}: {err}")
            self.data = deque()
            self.node = None


class StoreManager:
    def __init__(self, data_dir=DATA_DIR, window_days=7):
        self.data_dir = data_dir
        self.window_days = window_days
        self.stores = {}
        self.lock = threading.Lock()

        os.makedirs(self.data_dir, exist_ok=True)

    def get_store(self, node_id):
        node_id = safe_id(node_id)

        with self.lock:
            if node_id not in self.stores:
                file_path = os.path.join(self.data_dir, f"{node_id}.json")
                self.stores[node_id] = TimeWindowStore(
                    file_path=file_path,
                    window_days=self.window_days,
                )

            return self.stores[node_id]

    def known_nodes(self):
        nodes = set(self.stores.keys())

        if os.path.exists(self.data_dir):
            for filename in os.listdir(self.data_dir):
                if filename.endswith(".json"):
                    nodes.add(filename[:-5])

        return sorted(nodes)


bp = Blueprint("main", __name__)
store_manager = StoreManager(window_days=7)


@bp.route("/", methods=["GET"])
def index():
    return render_template("index.html")


@bp.route("/node/<node_id>", methods=["GET"])
def node_dashboard(node_id):
    return render_template("index.html")


@bp.route("/esp32_bme280", methods=["POST"])
def post_data():
    data = request.get_json(silent=True)

    if not data:
        return jsonify({"status": "error", "message": "Missing JSON body"}), 400

    if "node" not in data or "sample" not in data:
        return jsonify({"status": "error", "message": "Missing node or sample"}), 400

    node = data["node"]
    sample = data["sample"]

    try:
        node_id = make_node_id(node)
        store = store_manager.get_store(node_id)

        print(
            f"Received from {node_id}: "
            f"{sample.get('ts')}: "
            f"{sample.get('t')}, {sample.get('h')}, {sample.get('p')}"
        )

        logs = data.get("logs", [])

        store.add(
            temp=sample["t"],
            humidity=sample["h"],
            pressure=sample["p"],
            timestamp=sample.get("ts"),
            node=node,
            logs=logs,
        )

        return jsonify({
            "status": "ok",
            "node_id": node_id,
            "samples": len(store),
        })

    except Exception as err:
        print(f"POST /esp_ps failed: {err}")
        return jsonify({"status": "error", "message": str(err)}), 400

@bp.route("/nodes", methods=["GET"])
def list_nodes():
    return jsonify(store_manager.known_nodes())


@bp.route("/nodes/<node_id>/data", methods=["GET"])
def get_data(node_id):
    store = store_manager.get_store(node_id)
    return jsonify(store.get_all())


@bp.route("/nodes/<node_id>/info", methods=["GET"])
def get_node(node_id):
    store = store_manager.get_store(node_id)

    node = store.get_node()

    if node is None:
        return jsonify({}), 404

    info = dict(node)
    info["logs"] = list(store.logs)
    return jsonify(info)