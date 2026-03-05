import json

from django.utils import timezone
from rest_framework.views import APIView
from rest_framework.response import Response
from rest_framework import status
from .models import Bike, BikeStatus, AccidentLog
from .serializers import BikeSerializer, BikeStatusSerializer, AccidentLogSerializer
from asgiref.sync import async_to_sync
from channels.layers import get_channel_layer


from rest_framework.permissions import AllowAny
# ...
class UpdateBikeStatus(APIView):
    authentication_classes = []
    permission_classes = []
    
    """
    ESP32 posts sensor data + GPS here every 10 seconds.
    """
    def post(self, request):
        bike_id = request.data.get('bike_id')
        lat = request.data.get('lat', 0)
        lng = request.data.get('lng', 0)
        crashed = request.data.get('is_crashed', False)
        silent_mode = request.data.get('silent_mode', None)
        emergency_number = request.data.get('emergency_number', None)
        vibration = request.data.get('vibration', 0)
        fsr = request.data.get('fsr', 0)
        impact_force = request.data.get('impact_force', 0)
        tilt_angle = request.data.get('tilt_angle', 0)

        try:
            bike = Bike.objects.get(bike_id=bike_id)

            # Update bike config if provided
            if silent_mode is not None:
                bike.silent_mode = silent_mode
            if emergency_number:
                bike.emergency_number = emergency_number
            bike.is_online = True
            bike.last_heartbeat = timezone.now()
            bike.save()

            # Create status record
            bike_status = BikeStatus.objects.create(
                bike=bike,
                latitude=lat,
                longitude=lng,
                is_crashed=crashed,
                vibration=vibration,
                fsr=fsr,
                impact_force=impact_force,
                tilt_angle=tilt_angle,
            )

            # If crash detected, log it as an accident
            if crashed:
                AccidentLog.objects.create(
                    bike=bike,
                    scenario=4,  # Major crash by default
                    latitude=lat,
                    longitude=lng,
                    sms_sent=True,
                    call_made=True,
                    emergency_number_used=bike.emergency_number,
                    notes="Crash detected by ESP32 sensors",
                )

            # Broadcast to WebSocket
            try:
                channel_layer = get_channel_layer()
                async_to_sync(channel_layer.group_send)(
                    f"bike_{bike_id}",
                    {
                        "type": "bike_update",
                        "data": BikeStatusSerializer(bike_status).data
                    }
                )
            except Exception as e:
                print(f"WebSocket broadcast error: {e}")

            return Response({"status": "Success"}, status=status.HTTP_201_CREATED)

        except Bike.DoesNotExist:
            # Auto-create bike if it doesn't exist
            bike = Bike.objects.create(
                bike_id=bike_id,
                owner_name="Auto-registered",
                emergency_number=emergency_number or '+910000000000',
            )
            BikeStatus.objects.create(
                bike=bike, latitude=lat, longitude=lng, is_crashed=crashed
            )
            return Response({"status": "Bike auto-registered"}, status=status.HTTP_201_CREATED)


class HeartbeatView(APIView):
    authentication_classes = []
    permission_classes = []
    
    """
    ESP32 sends heartbeat every 5 minutes.
    Returns current config so ESP32 can sync any remote changes.
    """
    def post(self, request):
        bike_id = request.data.get('bike_id')

        try:
            bike = Bike.objects.get(bike_id=bike_id)
            bike.is_online = True
            bike.last_heartbeat = timezone.now()
            bike.save()

            return Response({
                "status": "alive",
                "emergency_number": bike.emergency_number,
                "emergency_name": bike.emergency_name,
                "silent_mode": bike.silent_mode,
            })

        except Bike.DoesNotExist:
            return Response({"error": "Bike not found"}, status=status.HTTP_404_NOT_FOUND)


class GetConfigView(APIView):
    authentication_classes = []
    permission_classes = []
    
    """
    ESP32 fetches config on boot (GET request).
    """
    def get(self, request):
        bike_id = request.query_params.get('bike_id')

        try:
            bike = Bike.objects.get(bike_id=bike_id)
            return Response({
                "emergency_number": bike.emergency_number,
                "emergency_name": bike.emergency_name,
                "silent_mode": bike.silent_mode,
            })
        except Bike.DoesNotExist:
            return Response({"error": "Bike not found"}, status=status.HTTP_404_NOT_FOUND)


class UpdateConfigView(APIView):
    authentication_classes = []
    permission_classes = []
    
    """
    Flutter app updates bike config (emergency number, silent mode).
    This syncs to Django so ESP32 can pick it up on next heartbeat.
    """
    def post(self, request):
        bike_id = request.data.get('bike_id')
        emergency_number = request.data.get('emergency_number')
        emergency_name = request.data.get('emergency_name')
        silent_mode = request.data.get('silent_mode')

        try:
            bike = Bike.objects.get(bike_id=bike_id)
            if emergency_number:
                bike.emergency_number = emergency_number
            if emergency_name:
                bike.emergency_name = emergency_name
            if silent_mode is not None:
                bike.silent_mode = silent_mode
            bike.save()

            return Response({
                "status": "Config updated",
                "emergency_number": bike.emergency_number,
                "emergency_name": bike.emergency_name,
                "silent_mode": bike.silent_mode,
            })
        except Bike.DoesNotExist:
            return Response({"error": "Bike not found"}, status=status.HTTP_404_NOT_FOUND)


class CancelEmergencyView(APIView):
    authentication_classes = []
    permission_classes = []
    
    """
    When rider presses 'I'm Okay' - cancel the active emergency.
    """
    def post(self, request):
        bike_id = request.data.get('bike_id')

        try:
            bike = Bike.objects.get(bike_id=bike_id)

            # Mark the latest accident as cancelled
            latest_accident = AccidentLog.objects.filter(
                bike=bike, was_cancelled=False
            ).first()

            if latest_accident:
                latest_accident.was_cancelled = True
                latest_accident.scenario = 3  # Rider was conscious
                latest_accident.notes = "Cancelled by rider via app/button"
                latest_accident.save()

            # Broadcast cancel to WebSocket
            try:
                channel_layer = get_channel_layer()
                async_to_sync(channel_layer.group_send)(
                    f"bike_{bike_id}",
                    {
                        "type": "bike_update",
                        "data": {
                            "is_crashed": False,
                            "cancelled": True,
                            "message": "Emergency cancelled by rider"
                        }
                    }
                )
            except Exception:
                pass

            return Response({"status": "Emergency cancelled"})

        except Bike.DoesNotExist:
            return Response({"error": "Bike not found"}, status=status.HTTP_404_NOT_FOUND)


class GetLatestStatusView(APIView):
    authentication_classes = []
    permission_classes = []
    
    """
    Flutter app fetches the latest status for the bike (for remote tracking).
    """
    def get(self, request):
        bike_id = request.query_params.get('bike_id')

        try:
            bike = Bike.objects.get(bike_id=bike_id)
            latest = BikeStatus.objects.filter(bike=bike).first()

            if latest:
                return Response({
                    "bike_id": bike.bike_id,
                    "owner_name": bike.owner_name,
                    "is_online": bike.is_online,
                    "emergency_number": bike.emergency_number,
                    "emergency_name": bike.emergency_name,
                    "silent_mode": bike.silent_mode,
                    "latest_status": BikeStatusSerializer(latest).data,
                })
            else:
                return Response({
                    "bike_id": bike.bike_id,
                    "owner_name": bike.owner_name,
                    "is_online": bike.is_online,
                    "latest_status": None,
                })

        except Bike.DoesNotExist:
            return Response({"error": "Bike not found"}, status=status.HTTP_404_NOT_FOUND)


class AccidentHistoryView(APIView):
    authentication_classes = []
    permission_classes = []
    
    """
    Get accident history for a bike.
    """
    def get(self, request):
        bike_id = request.query_params.get('bike_id')

        try:
            bike = Bike.objects.get(bike_id=bike_id)
            accidents = AccidentLog.objects.filter(bike=bike).order_by('-timestamp')[:50]
            serializer = AccidentLogSerializer(accidents, many=True)
            return Response(serializer.data)

        except Bike.DoesNotExist:
            return Response({"error": "Bike not found"}, status=status.HTTP_404_NOT_FOUND)


class RegisterBikeView(APIView):
    authentication_classes = []
    permission_classes = []
    
    """
    Register a new bike from the Flutter app.
    """
    def post(self, request):
        bike_id = request.data.get('bike_id')
        owner_name = request.data.get('owner_name', 'User')
        emergency_number = request.data.get('emergency_number', '+910000000000')
        emergency_name = request.data.get('emergency_name', 'Emergency Contact')

        bike, created = Bike.objects.get_or_create(
            bike_id=bike_id,
            defaults={
                'owner_name': owner_name,
                'emergency_number': emergency_number,
                'emergency_name': emergency_name,
            }
        )

        if not created:
            bike.owner_name = owner_name
            bike.emergency_number = emergency_number
            bike.emergency_name = emergency_name
            bike.save()

        return Response({
            "status": "registered" if created else "updated",
            "bike": BikeSerializer(bike).data,
        }, status=status.HTTP_201_CREATED if created else status.HTTP_200_OK)
