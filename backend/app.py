from pathlib import Path
from datetime import datetime, timezone
from threading import Lock
import base64
import json
import os

import cv2
from flask import Flask, jsonify, request, session
from flask_cors import CORS
from flask_socketio import SocketIO, emit, join_room, leave_room
from werkzeug.security import check_password_hash, generate_password_hash


BASE_DIR = Path(__file__).resolve().parent
DATA_DIR = BASE_DIR / "data"
USERS_FILE = DATA_DIR / "users.json"


app = Flask(__name__)
app.secret_key = os.environ.get("SECRET_KEY", "dev-secret-key-change-me")
app.config.update(
    SESSION_COOKIE_HTTPONLY=True,
    SESSION_COOKIE_SAMESITE="Lax",
)
CORS(
    app,
    supports_credentials=True,
    origins=[
        "http://localhost:5173",
        "http://127.0.0.1:5173",
    ],
)
socketio = SocketIO(
    app,
    async_mode="threading",
    cors_allowed_origins=[
        "http://localhost:5173",
        "http://127.0.0.1:5173",
    ],
)

motion_signal_state: dict[str, str | bool | None] = {
    "detected": False,
    "detected_at": None,
    "source": None,
}

DEFAULT_STREAM_INTERVAL_MS = 66
MIN_STREAM_INTERVAL_MS = 33
MAX_STREAM_INTERVAL_MS = 500

stream_state_lock = Lock()
stream_subscribers: set[str] = set()
stream_interval_ms = DEFAULT_STREAM_INTERVAL_MS
stream_worker_active = False


def ensure_storage() -> None:
    DATA_DIR.mkdir(exist_ok=True)
    if not USERS_FILE.exists():
        USERS_FILE.write_text("[]", encoding="utf-8")


def load_users() -> list[dict[str, str]]:
    ensure_storage()
    try:
        return json.loads(USERS_FILE.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return []


def save_users(users: list[dict[str, str]]) -> None:
    ensure_storage()
    USERS_FILE.write_text(json.dumps(users, indent=2), encoding="utf-8")


def find_user(username: str) -> dict[str, str] | None:
    return next((user for user in load_users() if user["username"] == username), None)


def public_user(user: dict[str, str]) -> dict[str, str]:
    return {
        "name": user["name"],
        "username": user["username"],
    }


def require_authenticated_user() -> tuple[dict[str, str] | None, tuple[dict[str, str], int] | None]:
    username = session.get("username")
    if not username:
        return None, ({"message": "Authentication required"}, 401)

    user = find_user(username)
    if user is None:
        session.pop("username", None)
        return None, ({"message": "Authentication required"}, 401)

    return user, None


def current_utc_timestamp() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def clamp_interval_ms(value: int) -> int:
    return max(MIN_STREAM_INTERVAL_MS, min(value, MAX_STREAM_INTERVAL_MS))


def encode_frame_as_data_url(frame) -> str | None:
    success, encoded = cv2.imencode(".jpg", frame, [int(cv2.IMWRITE_JPEG_QUALITY), 70])
    if not success:
        return None

    frame_b64 = base64.b64encode(encoded.tobytes()).decode("ascii")
    return f"data:image/jpeg;base64,{frame_b64}"


def get_stream_interval_ms() -> int:
    with stream_state_lock:
        return stream_interval_ms


def camera_stream_loop() -> None:
    global stream_worker_active

    camera = cv2.VideoCapture(0, cv2.CAP_DSHOW)
    if not camera.isOpened():
        camera.release()
        camera = cv2.VideoCapture(0)

    if not camera.isOpened():
        socketio.emit(
            "camera_stream_error",
            {"message": "Could not open laptop camera for simulation."},
        )
        with stream_state_lock:
            stream_worker_active = False
        return

    camera.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
    camera.set(cv2.CAP_PROP_FRAME_HEIGHT, 360)

    try:
        while True:
            with stream_state_lock:
                has_subscribers = len(stream_subscribers) > 0

            if not has_subscribers:
                break

            ok, frame = camera.read()
            if not ok:
                socketio.sleep(0.1)
                continue

            frame_payload = encode_frame_as_data_url(frame)
            if frame_payload is not None:
                socketio.emit(
                    "camera_frame",
                    {
                        "image": frame_payload,
                        "captured_at": current_utc_timestamp(),
                    },
                    to="camera_stream",
                )

            socketio.sleep(get_stream_interval_ms() / 1000)
    finally:
        camera.release()
        with stream_state_lock:
            stream_worker_active = False


@app.route("/api/health")
def health():
    return jsonify({"status": "ok"})


@app.route("/api/register", methods=["POST"])
def register():
    payload = request.get_json(silent=True) or {}
    name = payload.get("name", "").strip()
    username = payload.get("username", "").strip()
    password = payload.get("password", "")

    if not name or not username or not password:
        return jsonify({"message": "Name, username and password are required"}), 400

    users = load_users()
    if find_user(username) is not None:
        return jsonify({"message": "Username already taken"}), 409

    new_user = {
        "name": name,
        "username": username,
        "password_hash": generate_password_hash(password),
    }
    users.append(new_user)
    save_users(users)
    session["username"] = username

    return jsonify({"user": public_user(new_user)}), 201


@app.route("/api/login", methods=["POST"])
def login():
    payload = request.get_json(silent=True) or {}
    username = payload.get("username", "").strip()
    password = payload.get("password", "")

    if not username or not password:
        return jsonify({"message": "Username and password are required"}), 400

    user = find_user(username)
    if user is None or not check_password_hash(user["password_hash"], password):
        return jsonify({"message": "Invalid username or password"}), 401

    session["username"] = username
    return jsonify({"user": public_user(user)})


@app.route("/api/me")
def me():
    user, auth_error = require_authenticated_user()
    if auth_error:
        message, status = auth_error
        return jsonify(message), status

    return jsonify({"user": public_user(user)})


@app.route("/api/camera/move", methods=["POST"])
def camera_move():
    user, auth_error = require_authenticated_user()
    if auth_error:
        message, status = auth_error
        return jsonify(message), status

    payload = request.get_json(silent=True) or {}
    direction = str(payload.get("direction", "")).strip().lower()
    allowed_directions = {"up", "down", "left", "right"}

    if direction not in allowed_directions:
        return jsonify({"message": "Direction must be one of: up, down, left, right"}), 400

    # Placeholder for future camera hardware integration.
    app.logger.info("Camera move requested by %s: %s", user["username"], direction)
    socketio.emit(
        "camera_command",
        {
            "direction": direction,
            "requested_by": user["username"],
            "at": current_utc_timestamp(),
        },
    )
    return jsonify({"message": "Move command received", "direction": direction})


@app.route("/api/buzzer", methods=["POST"])
def buzzer_beep():
    user, auth_error = require_authenticated_user()
    if auth_error:
        message, status = auth_error
        return jsonify(message), status

    # Placeholder for future buzzer hardware integration.
    app.logger.info("Buzzer beep requested by %s", user["username"])
    socketio.emit(
        "buzzer_signal",
        {
            "requested_by": user["username"],
            "at": current_utc_timestamp(),
        },
    )
    return jsonify({"message": "Buzzer signal received"})


@app.route("/api/motion", methods=["GET"])
def get_motion_signal():
    user, auth_error = require_authenticated_user()
    if auth_error:
        message, status = auth_error
        return jsonify(message), status

    return jsonify(motion_signal_state)


@app.route("/api/motion", methods=["POST"])
def set_motion_signal():
    payload = request.get_json(silent=True) or {}
    detected = bool(payload.get("detected", True))
    source = str(payload.get("source", "sensor")).strip() or "sensor"

    # Placeholder endpoint for future hardware sensor integration.
    motion_signal_state["detected"] = detected
    motion_signal_state["source"] = source
    motion_signal_state["detected_at"] = None if not detected else current_utc_timestamp()
    socketio.emit("motion_signal", motion_signal_state)

    app.logger.info("Motion signal received: detected=%s source=%s", detected, source)
    return jsonify({"message": "Motion signal updated", **motion_signal_state})


@socketio.on("connect")
def on_connect():
    if not session.get("username"):
        return False

    emit("motion_signal", motion_signal_state)


@socketio.on("subscribe_camera_stream")
def on_subscribe_camera_stream(payload=None):
    global stream_worker_active, stream_interval_ms

    user, auth_error = require_authenticated_user()
    if auth_error:
        return {"ok": False, "message": "Authentication required"}

    payload = payload or {}
    requested_interval_ms = payload.get("interval_ms", DEFAULT_STREAM_INTERVAL_MS)
    try:
        requested_interval_ms = int(requested_interval_ms)
    except (TypeError, ValueError):
        requested_interval_ms = DEFAULT_STREAM_INTERVAL_MS

    requested_interval_ms = clamp_interval_ms(requested_interval_ms)

    with stream_state_lock:
        stream_subscribers.add(request.sid)
        stream_interval_ms = requested_interval_ms
        should_start_worker = not stream_worker_active
        if should_start_worker:
            stream_worker_active = True

    join_room("camera_stream")

    if should_start_worker:
        socketio.start_background_task(camera_stream_loop)
    app.logger.info(
        "Camera stream subscribed by %s (sid=%s, interval=%sms)",
        user["username"],
        request.sid,
        requested_interval_ms,
    )
    return {"ok": True, "interval_ms": requested_interval_ms}


@socketio.on("unsubscribe_camera_stream")
def on_unsubscribe_camera_stream(_payload=None):
    with stream_state_lock:
        stream_subscribers.discard(request.sid)

    leave_room("camera_stream")
    return {"ok": True}


@socketio.on("disconnect")
def on_disconnect():
    with stream_state_lock:
        stream_subscribers.discard(request.sid)


@socketio.on("camera_move")
def on_camera_move(payload):
    user, auth_error = require_authenticated_user()
    if auth_error:
        return {"ok": False, "message": "Authentication required"}

    payload = payload or {}
    direction = str(payload.get("direction", "")).strip().lower()
    allowed_directions = {"up", "down", "left", "right"}
    if direction not in allowed_directions:
        return {"ok": False, "message": "Direction must be one of: up, down, left, right"}

    app.logger.info("Camera move requested by %s via socket: %s", user["username"], direction)
    socketio.emit(
        "camera_command",
        {
            "direction": direction,
            "requested_by": user["username"],
            "at": current_utc_timestamp(),
        },
    )
    return {"ok": True, "direction": direction}


@socketio.on("trigger_buzzer")
def on_trigger_buzzer(_payload=None):
    user, auth_error = require_authenticated_user()
    if auth_error:
        return {"ok": False, "message": "Authentication required"}

    app.logger.info("Buzzer trigger requested by %s via socket", user["username"])
    socketio.emit(
        "buzzer_signal",
        {
            "requested_by": user["username"],
            "at": current_utc_timestamp(),
        },
    )
    return {"ok": True}


@app.route("/api/logout", methods=["POST"])
def logout():
    session.pop("username", None)
    return jsonify({"message": "Logged out"})


if __name__ == "__main__":
    socketio.run(app, debug=True, port=5000)