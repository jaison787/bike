# ----------- BACKEND: Django Models (models.py) -----------
from django.db import models

class Bike(models.Model):
    bike_id = models.CharField(max_length=50, unique=True)
    owner_name = models.CharField(max_length=100)
    is_active = models.BooleanField(default=True)
    created_at = models.DateTimeField(auto_now_add=True)

    def __str__(self):
        return f"{self.bike_id} ({self.owner_name})"

class BikeStatus(models.Model):
    bike = models.ForeignKey(Bike, on_delete=models.CASCADE, related_name='statuses')
    latitude = models.DecimalField(max_digits=9, decimal_places=6)
    longitude = models.DecimalField(max_digits=9, decimal_places=6)
    is_crashed = models.BooleanField(default=False)
    timestamp = models.DateTimeField(auto_now_add=True)

    class Meta:
        ordering = ['-timestamp']

# ----------- BACKEND: DRF Serializers (serializers.py) -----------
from rest_framework import serializers

class BikeStatusSerializer(serializers.ModelSerializer):
    class Meta:
        model = BikeStatus
        fields = '__all__'

# ----------- BACKEND: REST Views (views.py) -----------
from rest_framework.views import APIView
from rest_framework.response import Response
from .models import Bike, BikeStatus
from .serializers import BikeStatusSerializer
from asgiref.sync import async_to_sync
from channels.layers import get_channel_layer

class UpdateBikeStatus(APIView):
    def post(self, request):
        bike_id = request.data.get('bike_id')
        lat = request.data.get('lat')
        lng = request.data.get('lng')
        crashed = request.data.get('is_crashed')

        try:
            bike = Bike.objects.get(bike_id=bike_id)
            status = BikeStatus.objects.create(
                bike=bike, latitude=lat, longitude=lng, is_crashed=crashed
            )

            # BROADCAST TO WEBSOCKET (Django Channels)
            channel_layer = get_channel_layer()
            async_to_sync(channel_layer.group_send)(
                f"bike_{bike_id}",
                {
                    "type": "bike_update",
                    "data": BikeStatusSerializer(status).data
                }
            )

            return Response({"status": "Success"}, status=201)
        except Bike.DoesNotExist:
            return Response({"error": "Bike not found"}, status=404)

# ----------- BACKEND: WebSockets (consumers.py) -----------
import json
from channels.generic.websocket import AsyncWebsocketConsumer

class BikeConsumer(AsyncWebsocketConsumer):
    async def connect(self):
        self.bike_id = self.scope['url_route']['kwargs']['bike_id']
        self.group_name = f"bike_{self.bike_id}"

        # Join room group
        await self.channel_layer.group_add(self.group_name, self.channel_name)
        await self.accept()

    async def disconnect(self, close_code):
        await self.channel_layer.group_discard(self.group_name, self.channel_name)

    async def bike_update(self, event):
        # Send message to WebSocket
        await self.send(text_data=json.dumps(event['data']))
