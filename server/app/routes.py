from flask import Blueprint, request, jsonify

bp = Blueprint("main", __name__)

@bp.route("/esp_ps", methods=["GET", "POST"])
def post_data():
    if request.method == "POST":
        data = request.get_json(silent=True)
        print(f"Received: {data['sample']['ts']}: {data['sample']['t']}, {data['sample']['h']}, {data['sample']['p']}")
        print(f"Node: {data['node']})")
        return jsonify({"status": "ok"})