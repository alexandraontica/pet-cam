from pathlib import Path
import json
import os

from flask import Flask, jsonify, request, session
from flask_cors import CORS
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
    return jsonify({"message": "Move command received", "direction": direction})


@app.route("/api/logout", methods=["POST"])
def logout():
    session.pop("username", None)
    return jsonify({"message": "Logged out"})


if __name__ == "__main__":
    app.run(debug=True, port=5000)