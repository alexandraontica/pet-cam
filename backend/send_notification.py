import requests

TOKEN = "8788919866:AAGUE_egY_oY7seqdATUTbgzNGW0Qzkjt6w"
CHAT_ID = "8731238656"

def send_notification(message):
    """Send a simple text message via the Telegram Bot API."""
    url = f"https://api.telegram.org/bot{TOKEN}/sendMessage"
    payload = {
        "chat_id": CHAT_ID,
        "text": message,
        "parse_mode": "Markdown"
    }
    try:
        response = requests.post(url, json=payload)
        return response.json()
    except Exception as e:
        print(f"Error sending text message: {e}")

def send_photo(photo_path, caption=""):
    """Send an image captured by the camera."""
    url = f"https://api.telegram.org/bot{TOKEN}/sendPhoto"
    try:
        with open(photo_path, 'rb') as photo:
            files = {'photo': photo}
            data = {'chat_id': CHAT_ID, 'caption': caption}
            response = requests.post(url, files=files, data=data)
            return response.json()
    except Exception as e:
        print(f"Error sending photo: {e}")
