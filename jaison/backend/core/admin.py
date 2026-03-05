from django.contrib import admin
from .models import Bike, BikeStatus, AccidentLog


@admin.register(Bike)
class BikeAdmin(admin.ModelAdmin):
    list_display = ['bike_id', 'owner_name', 'emergency_number', 'is_active', 'is_online', 'silent_mode', 'last_heartbeat']
    list_filter = ['is_active', 'is_online', 'silent_mode']
    search_fields = ['bike_id', 'owner_name']


@admin.register(BikeStatus)
class BikeStatusAdmin(admin.ModelAdmin):
    list_display = ['bike', 'latitude', 'longitude', 'is_crashed', 'vibration', 'fsr', 'impact_force', 'timestamp']
    list_filter = ['is_crashed', 'bike']
    ordering = ['-timestamp']


@admin.register(AccidentLog)
class AccidentLogAdmin(admin.ModelAdmin):
    list_display = ['bike', 'scenario', 'was_cancelled', 'sms_sent', 'call_made', 'emergency_number_used', 'timestamp']
    list_filter = ['scenario', 'was_cancelled', 'bike']
    ordering = ['-timestamp']
