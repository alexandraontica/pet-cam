from pathlib import Path
from datetime import datetime, timezone
from threading import Lock
import base64
import json
import os

from flask import Flask, Response, jsonify, request, session
from flask_cors import CORS
from flask_socketio import SocketIO, emit, join_room, leave_room
from werkzeug.security import check_password_hash, generate_password_hash

from send_notification import send_notification, send_photo


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

notifications_enabled = True

# Camera frame buffer (populated by ESP32-CAM via HTTP POST) 
frame_lock = Lock()
latest_frame_jpeg: bytes | None = None        # Raw JPEG bytes from ESP32
latest_frame_data_url: str | None = None       # Base64 data URL for Socket.IO
latest_frame_timestamp: str | None = None

stream_subscribers: set[str] = set()
stream_state_lock = Lock()


def ensure_storage() -> None:
    """Create the data directory and users file if they don't exist."""
    DATA_DIR.mkdir(exist_ok=True)
    if not USERS_FILE.exists():
        USERS_FILE.write_text("[]", encoding="utf-8")


def load_users() -> list[dict[str, str]]:
    """Read and return the list of users from the JSON file."""
    ensure_storage()
    try:
        return json.loads(USERS_FILE.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return []


def save_users(users: list[dict[str, str]]) -> None:
    """Write the users list to the JSON file."""
    ensure_storage()
    USERS_FILE.write_text(json.dumps(users, indent=2), encoding="utf-8")


def find_user(username: str) -> dict[str, str] | None:
    """Find and return a user by username, or None if not found."""
    return next((user for user in load_users() if user["username"] == username), None)


def public_user(user: dict[str, str]) -> dict[str, str]:
    """Return a safe subset of user data (without password hash)."""
    return {
        "name": user["name"],
        "username": user["username"],
    }


def require_authenticated_user() -> tuple[dict[str, str] | None, tuple[dict[str, str], int] | None]:
    """Check session for an authenticated user. Returns (user, None) or (None, error_response)."""
    username = session.get("username")
    if not username:
        return None, ({"message": "Authentication required"}, 401)

    user = find_user(username)
    if user is None:
        session.pop("username", None)
        return None, ({"message": "Authentication required"}, 401)

    return user, None


def current_utc_timestamp() -> str:
    """Return the current UTC time as an ISO 8601 string."""
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def store_and_broadcast_frame(jpeg_bytes: bytes) -> None:
    """Store the latest JPEG frame and broadcast to all stream subscribers."""
    global latest_frame_jpeg, latest_frame_data_url, latest_frame_timestamp

    frame_b64 = base64.b64encode(jpeg_bytes).decode("ascii")
    data_url = f"data:image/jpeg;base64,{frame_b64}"
    timestamp = current_utc_timestamp()

    with frame_lock:
        latest_frame_jpeg = jpeg_bytes
        latest_frame_data_url = data_url
        latest_frame_timestamp = timestamp

    # Broadcast to all subscribed frontend clients
    with stream_state_lock:
        has_subscribers = len(stream_subscribers) > 0

    if has_subscribers:
        socketio.emit(
            "camera_frame",
            {"image": data_url, "captured_at": timestamp},
            to="camera_stream",
        )


@app.route("/api/health")
def health():
    """Simple health check endpoint."""
    return jsonify({"status": "ok"})


@app.route("/api/camera/frame", methods=["POST"])
def receive_camera_frame():
    """Receive a JPEG frame from the ESP32-CAM."""
    jpeg_bytes = request.get_data()
    if not jpeg_bytes:
        return jsonify({"message": "No image data received"}), 400

    # Basic JPEG validation (starts with FF D8)
    if len(jpeg_bytes) < 2 or jpeg_bytes[0] != 0xFF or jpeg_bytes[1] != 0xD8:
        return jsonify({"message": "Invalid JPEG data"}), 400

    store_and_broadcast_frame(jpeg_bytes)
    return jsonify({"ok": True})


@app.route("/api/camera/latest")
def get_latest_frame():
    """Return the latest JPEG frame (useful for debugging)."""
    with frame_lock:
        frame = latest_frame_jpeg

    if frame is None:
        return jsonify({"message": "No frame available yet"}), 404

    return Response(frame, mimetype="image/jpeg")


@app.route("/api/register", methods=["POST"])
def register():
    """Register a new user with name, username and password."""
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
    """Authenticate a user and create a session."""
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
    """Return the currently authenticated user's public info."""
    user, auth_error = require_authenticated_user()
    if auth_error:
        message, status = auth_error
        return jsonify(message), status

    return jsonify({"user": public_user(user)})


@app.route("/api/camera/move", methods=["POST"])
def camera_move():
    """Send a camera movement command (up/down/left/right) to the ESP32."""
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
    """Trigger the buzzer on the ESP32 to distract the pet."""
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
    """Return the current motion detection state."""
    user, auth_error = require_authenticated_user()
    if auth_error:
        message, status = auth_error
        return jsonify(message), status

    return jsonify(motion_signal_state)


@app.route("/api/motion", methods=["POST"])
def set_motion_signal():
    """Update the motion signal state (called by the ESP32 sensor)."""
    payload = request.get_json(silent=True) or {}
    detected = bool(payload.get("detected", True))
    source = str(payload.get("source", "sensor")).strip() or "sensor"

    # Placeholder endpoint for future hardware sensor integration.
    motion_signal_state["detected"] = detected
    motion_signal_state["source"] = source
    motion_signal_state["detected_at"] = None if not detected else current_utc_timestamp()
    socketio.emit("motion_signal", motion_signal_state)

    global notifications_enabled
    if detected and notifications_enabled:
        # Notify via Telegram
        send_notification("🚨 *Miscare detectata!*")
        
        # Save latest frame and send it as a photo if available
        with frame_lock:
            frame = latest_frame_jpeg
            
        if frame is not None:
            photo_path = DATA_DIR / "motion_snapshot.jpg"
            photo_path.write_bytes(frame)
            send_photo(str(photo_path), "Iata ultimul cadru capturat la momentul detectiei:")

    app.logger.info("Motion signal received: detected=%s source=%s", detected, source)
    return jsonify({"message": "Motion signal updated", **motion_signal_state})


@app.route("/api/settings/notifications", methods=["POST"])
def update_notifications_setting():
    """Update the backend notifications setting based on frontend switch."""
    global notifications_enabled
    payload = request.get_json(silent=True) or {}
    notifications_enabled = bool(payload.get("enabled", True))
    app.logger.info("Notifications enabled set to: %s", notifications_enabled)
    return jsonify({"message": "Settings updated", "enabled": notifications_enabled})


@socketio.on("connect")
def on_connect():
    """Handle new Socket.IO connection. Reject unauthenticated clients."""
    if not session.get("username"):
        return False

    emit("motion_signal", motion_signal_state)


@socketio.on("subscribe_camera_stream")
def on_subscribe_camera_stream(payload=None):
    """Subscribe the client to the live camera stream room."""
    user, auth_error = require_authenticated_user()
    if auth_error:
        return {"ok": False, "message": "Authentication required"}

    with stream_state_lock:
        stream_subscribers.add(request.sid)

    join_room("camera_stream")

    # Send the latest frame immediately so the client doesn't see a blank screen
    with frame_lock:
        data_url = latest_frame_data_url
        ts = latest_frame_timestamp

    if data_url is not None:
        emit("camera_frame", {"image": data_url, "captured_at": ts})

    app.logger.info(
        "Camera stream subscribed by %s (sid=%s)",
        user["username"],
        request.sid,
    )
    return {"ok": True}


@socketio.on("unsubscribe_camera_stream")
def on_unsubscribe_camera_stream(_payload=None):
    """Unsubscribe the client from the camera stream room."""
    with stream_state_lock:
        stream_subscribers.discard(request.sid)

    leave_room("camera_stream")
    return {"ok": True}


@socketio.on("disconnect")
def on_disconnect():
    """Clean up stream subscription when a client disconnects."""
    with stream_state_lock:
        stream_subscribers.discard(request.sid)


@socketio.on("camera_move")
def on_camera_move(payload):
    """Handle camera move command received via Socket.IO."""
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
    """Handle buzzer trigger command received via Socket.IO."""
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
    """End the current user session."""
    session.pop("username", None)
    return jsonify({"message": "Logged out"})


if __name__ == "__main__":
    socketio.run(app, debug=True, host="0.0.0.0", port=5000)