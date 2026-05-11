"""
Management command: python manage.py seed_bike
Creates BIKE001 in the database if it doesn't exist.
Safe to run multiple times -- won't overwrite existing data.
"""
from django.core.management.base import BaseCommand
from core.models import Bike


class Command(BaseCommand):
    help = 'Creates BIKE001 in the database if it does not already exist'

    def handle(self, *args, **options):
        bike, created = Bike.objects.get_or_create(
            bike_id='BIKE001',
            defaults={
                'owner_name': 'SmartBike Rider',
                'emergency_number': '+917902202367',
                'emergency_name': 'Emergency Contact',
                'silent_mode': False,
                'is_active': True,
                'is_online': False,
            }
        )
        if created:
            self.stdout.write(self.style.SUCCESS(
                '[OK] Created BIKE001 (id={}) in database'.format(bike.pk)
            ))
        else:
            self.stdout.write(self.style.WARNING(
                '[INFO] BIKE001 already exists (id={}) -- no changes made'.format(bike.pk)
            ))
