from django.urls import re_path
from . import consumers

websocket_urlpatterns = [
    re_path(r'ws/bike/(?P<bike_id>\w+)/$', consumers.BikeConsumer.as_asgi()),
]
