import os
import django

os.environ['DJANGO_SETTINGS_MODULE'] = 'bike_backend.settings'
django.setup()

from core.models import Bike

bike, created = Bike.objects.get_or_create(
    bike_id='BIKE_001',
    defaults={
        'owner_name': 'Jaison',
        'emergency_number': '+919994235648',
        'emergency_name': 'Emergency Contact',
    }
)

if created:
    print(f"Bike '{bike.bike_id}' created successfully!")
else:
    print(f"Bike '{bike.bike_id}' already exists. Updating...")
    bike.owner_name = 'Jaison'
    bike.emergency_number = '+919994235648'
    bike.save()
    print("Updated!")

print(f"  Owner: {bike.owner_name}")
print(f"  Emergency: {bike.emergency_number}")
print(f"  Active: {bike.is_active}")
