from django.db import models


class Bike(models.Model):
    bike_id = models.CharField(max_length=50, unique=True)
    owner_name = models.CharField(max_length=100)
    emergency_number = models.CharField(max_length=20, default='+910000000000')
    emergency_name = models.CharField(max_length=100, default='Emergency Contact')
    silent_mode = models.BooleanField(default=False)
    is_active = models.BooleanField(default=True)
    is_online = models.BooleanField(default=False)
    last_heartbeat = models.DateTimeField(null=True, blank=True)
    created_at = models.DateTimeField(auto_now_add=True)

    def __str__(self):
        return f"{self.bike_id} ({self.owner_name})"

    class Meta:
        verbose_name = 'Bike'
        verbose_name_plural = 'Bikes'


class BikeStatus(models.Model):
    bike = models.ForeignKey(Bike, on_delete=models.CASCADE, related_name='statuses')
    latitude = models.DecimalField(max_digits=10, decimal_places=6, default=0)
    longitude = models.DecimalField(max_digits=10, decimal_places=6, default=0)
    is_crashed = models.BooleanField(default=False)
    vibration = models.IntegerField(default=0)
    fsr = models.IntegerField(default=0)
    impact_force = models.FloatField(default=0)
    tilt_angle = models.FloatField(default=0)
    timestamp = models.DateTimeField(auto_now_add=True)

    class Meta:
        ordering = ['-timestamp']
        verbose_name = 'Bike Status'
        verbose_name_plural = 'Bike Statuses'

    def __str__(self):
        return f"{self.bike.bike_id} @ {self.timestamp.strftime('%Y-%m-%d %H:%M:%S')}"


class AccidentLog(models.Model):
    SCENARIO_CHOICES = [
        (1, 'Parked Bike Hit'),
        (2, 'Minor Collision'),
        (3, 'Major Crash - Rider Conscious'),
        (4, 'Major Crash - Rider Unresponsive'),
    ]

    bike = models.ForeignKey(Bike, on_delete=models.CASCADE, related_name='accidents')
    scenario = models.IntegerField(choices=SCENARIO_CHOICES, default=4)
    latitude = models.DecimalField(max_digits=10, decimal_places=6, default=0)
    longitude = models.DecimalField(max_digits=10, decimal_places=6, default=0)
    was_cancelled = models.BooleanField(default=False)
    sms_sent = models.BooleanField(default=False)
    call_made = models.BooleanField(default=False)
    emergency_number_used = models.CharField(max_length=20, blank=True)
    notes = models.TextField(blank=True)
    timestamp = models.DateTimeField(auto_now_add=True)

    class Meta:
        ordering = ['-timestamp']
        verbose_name = 'Accident Log'
        verbose_name_plural = 'Accident Logs'

    def __str__(self):
        return f"Accident #{self.pk} - {self.bike.bike_id} - Scenario {self.scenario}"
