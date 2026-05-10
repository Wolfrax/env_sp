from flask import Blueprint, render_template, request, jsonify
from collections import deque
from datetime import datetime, timedelta, timezone
import json
import os

class TimeWindowStore:
    def __init__(self, file_path, window_days=7):
        self.file_path = file_path
        self.window = timedelta(days=window_days)
        self.node = None
        self.data = deque()
        self._load()

    # ---------- Public API ----------

    def add(self, temp, humidity, pressure, timestamp=None, node=None):
        if timestamp is None:
            timestamp = datetime.utcnow()
        elif isinstance(timestamp, str):
            timestamp = datetime.fromisoformat(timestamp.replace("Z", "+00:00"))

        self.node = node

        entry = {
            "t": float(temp),
            "h": float(humidity),
            "p": float(pressure),
            "ts": timestamp,
        }

        self.data.append(entry)
        self._cleanup(timestamp)
        self._save()

    def get_all(self):
        return list(self.data)

    def __len__(self):
        return len(self.data)

    # ---------- Internal ----------

    def _cleanup(self, reference_time):
        cutoff = reference_time - self.window
        while self.data and self.data[0]["ts"] < cutoff:
            self.data.popleft()

    def _serialize(self):
        return {
            "node": self.node,
            "data": [
                {
                    **d,
                    "ts": d["ts"].isoformat()
                }
                for d in self.data
            ]
        }

    def _deserialize(self, obj):
        self.node = obj.get("node")

        return deque({
            **d,
            "ts": datetime.fromisoformat(d["ts"])
        } for d in obj.get("data", []))

    def _save(self):
        tmp_file = self.file_path + ".tmp"
        with open(tmp_file, "w") as f:
            json.dump(self._serialize(), f, indent=2)
        os.replace(tmp_file, self.file_path)  # atomic write

    def _load(self):
        if not os.path.exists(self.file_path):
            self.data = deque()
            return

        try:
            with open(self.file_path, "r") as f:
                obj = json.load(f)
                self.data = self._deserialize(obj)

            now = datetime.now(timezone.utc)
            self._cleanup(now)

        except Exception:
            self.data = deque()
            self.node = None


bp = Blueprint("main", __name__)
esp_data = TimeWindowStore("esp_data.json", window_days=7)

@bp.route("/esp_ps", methods=["GET", "POST"])
def post_data():
    if request.method == "POST":
        data = request.get_json(silent=True)
        print(f"Received: {data['sample']['ts']}: {data['sample']['t']}, {data['sample']['h']}, {data['sample']['p']}")
        print(f"Node: {data['node']})")
        esp_data.add(data['sample']['t'], data['sample']['h'], data['sample']['p'], data['sample']['ts'], data['node'])
        return jsonify({"status": "ok"})
    else:
        return jsonify(esp_data.get_all())

@bp.route("/", methods=["GET"])
def index():
    return render_template("index.html")

@bp.route("/data", methods=["GET"])
def get_data():
    return jsonify(esp_data.get_all())

@bp.route("/get_node", methods=["GET"])
def get_node():
    return jsonify(esp_data.node)