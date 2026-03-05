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
