import requests

url = "http://xp1f2rd8-8003.inc1.devtunnels.ms/api/update_status/"
headers = {
    "X-Tunnel-Skip-Anti-Phishing-Page": "true",
    "User-Agent": "SmartBike/1.0 (ESP32)"
}

try:
    # We use allow_redirects=False to see if it tries to send us to HTTPS
    response = requests.post(url, json={"bike_id":"TEST"}, headers=headers, allow_redirects=False)
    print(f"Status Code: {response.status_code}")
    print(f"Headers: {dict(response.headers)}")
    if 'Location' in response.headers:
        print(f"Redirecting to: {response.headers['Location']}")
except Exception as e:
    print(f"Error: {e}")
