"""
URL configuration for bike_backend project.
"""
from django.contrib import admin
from django.urls import path
from core.views import (
    UpdateBikeStatus,
    HeartbeatView,
    GetConfigView,
    UpdateConfigView,
    CancelEmergencyView,
    GetLatestStatusView,
    AccidentHistoryView,
    RegisterBikeView,
)

urlpatterns = [
    path('admin/', admin.site.urls),

    # ESP32 endpoints
    path('api/update_status/', UpdateBikeStatus.as_view(), name='update_status'),
    path('api/heartbeat/', HeartbeatView.as_view(), name='heartbeat'),
    path('api/get_config/', GetConfigView.as_view(), name='get_config'),

    # Flutter app endpoints
    path('api/update_config/', UpdateConfigView.as_view(), name='update_config'),
    path('api/cancel_emergency/', CancelEmergencyView.as_view(), name='cancel_emergency'),
    path('api/latest_status/', GetLatestStatusView.as_view(), name='latest_status'),
    path('api/accident_history/', AccidentHistoryView.as_view(), name='accident_history'),
    path('api/register_bike/', RegisterBikeView.as_view(), name='register_bike'),
]
