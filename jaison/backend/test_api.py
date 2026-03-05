import requests
import json

url = "http://localhost:8000/api/update_status/"
data = {
    "bike_id": "BIKE_001",
    "lat": "10.688288",
    "lng": "76.059395",
    "is_crashed": False,
    "silent_mode": False,
    "emergency_number": "+919789639636",
    "vibration": "0",
    "fsr": "0",
    "impact_force": "16118.03",
    "tilt_angle": "2.23"
}

response = requests.post(url, json=data)
print(f"Status: {response.status_code}")
print(f"Response: {response.text}")
