from rest_framework import serializers
from .models import Bike, BikeStatus, AccidentLog


class BikeSerializer(serializers.ModelSerializer):
    class Meta:
        model = Bike
        fields = '__all__'


class BikeStatusSerializer(serializers.ModelSerializer):
    bike_id = serializers.CharField(source='bike.bike_id', read_only=True)

    class Meta:
        model = BikeStatus
        fields = ['id', 'bike_id', 'latitude', 'longitude', 'is_crashed',
                  'vibration', 'fsr', 'impact_force', 'tilt_angle', 'timestamp']


class AccidentLogSerializer(serializers.ModelSerializer):
    bike_id = serializers.CharField(source='bike.bike_id', read_only=True)
    scenario_display = serializers.CharField(source='get_scenario_display', read_only=True)

    class Meta:
        model = AccidentLog
        fields = ['id', 'bike_id', 'scenario', 'scenario_display', 'latitude', 'longitude',
                  'was_cancelled', 'sms_sent', 'call_made', 'emergency_number_used',
                  'notes', 'timestamp']
