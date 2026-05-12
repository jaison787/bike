/*
 * MOBILE APP: Smart Bike Dashboard (Flutter)
 * Features: Live Tracking, Real-time Alerts via WebSockets.
 */

import 'package:flutter/material.dart';
import 'package:google_maps_flutter/google_maps_flutter.dart';
import 'package:web_socket_channel/web_socket_channel.dart';
import 'dart:convert';
import 'package:audioplayers/audioplayers.dart'; // Add to pubspec.yaml

void main() => runApp(const MaterialApp(home: BikeDashboard()));

class BikeDashboard extends StatefulWidget {
    const BikeDashboard({super.key});

    @override
    State<BikeDashboard> createState() => _BikeDashboardState();
}

class _BikeDashboardState extends State<BikeDashboard> {
    GoogleMapController? mapController;
    WebSocketChannel? channel;
    LatLng bikePosition = const LatLng(12.9716, 77.5946); // Default (Bangalore)
    bool isCrashed = false;
    final String bikeId = "BIKE_001";
    final AudioPlayer _player = AudioPlayer();

    @override
    void initState() {
        super.initState();
        connectToWebSocket();
    }

    void connectToWebSocket() {
        // Connect to your Django server WebSocket
        channel = WebSocketChannel.connect(
            Uri.parse('ws://your-django-server.com/ws/bike/$bikeId/'),
        );

        channel!.stream.listen((message) {
            final data = json.decode(message);
            setState(() {
                bikePosition = LatLng(
                    double.parse(data['latitude']), 
                    double.parse(data['longitude'])
                );
                isCrashed = data['is_crashed'];
                
                if (isCrashed) {
                    triggerCrashAlert();
                }
            });
            
            // Move camera to bike position
            mapController?.animateCamera(
                CameraUpdate.newLatLng(bikePosition)
            );
        });
    }

    void triggerCrashAlert() {
        _player.play(AssetSource('emergency_alert.mp3'));
        showDialog(
            context: context,
            barrierDismissible: false,
            builder: (context) => AlertDialog(
                backgroundColor: Colors.red[900],
                title: const Icon(Icons.warning, color: Colors.white, size: 50),
                content: const Text(
                    "CRASH DETECTED!\nImmediate assistance may be required.",
                    textAlign: TextAlign.center,
                    style: TextStyle(color: Colors.white, fontSize: 18),
                ),
                actions: [
                    TextButton(
                        onPressed: () {
                            _player.stop();
                            Navigator.pop(context);
                        },
                        child: const Text("I AM SAFE", style: TextStyle(color: Colors.white)),
                    ),
                ],
            ),
        );
    }

    @override
    Widget build(BuildContext context) {
        return Scaffold(
            appBar: AppBar(
                title: const Text("SMART BIKE TRACKER"),
                backgroundColor: Colors.black,
                actions: [
                    Icon(Icons.circle, color: isCrashed ? Colors.red : Colors.green),
                    const SizedBox(width: 20),
                ],
            ),
            body: Stack(
                children: [
                    GoogleMap(
                        initialCameraPosition: CameraPosition(target: bikePosition, zoom: 15),
                        onMapCreated: (controller) => mapController = controller,
                        markers: {
                            Marker(
                                markerId: const MarkerId("bike"),
                                position: bikePosition,
                                icon: BitmapDescriptor.defaultMarkerWithHue(
                                    isCrashed ? BitmapDescriptor.hueRed : BitmapDescriptor.hueAzure
                                ),
                            ),
                        },
                    ),
                    Positioned(
                        bottom: 20,
                        left: 20,
                        right: 20,
                        child: Card(
                            elevation: 10,
                            color: Colors.black.withOpacity(0.8),
                            child: Padding(
                                padding: const EdgeInsets.all(16.0),
                                child: Column(
                                    mainAxisSize: MainAxisSize.min,
                                    children: [
                                        Text("Bike ID: $bikeId", style: const TextStyle(color: Colors.white)),
                                        const Divider(color: Colors.grey),
                                        Row(
                                            mainAxisAlignment: MainAxisAlignment.spaceAround,
                                            children: [
                                                Column(
                                                    children: [
                                                        const Text("LATITUDE", style: TextStyle(color: Colors.blue)),
                                                        Text(bikePosition.latitude.toStringAsFixed(4), style: const TextStyle(color: Colors.white)),
                                                    ],
                                                ),
                                                Column(
                                                    children: [
                                                        const Text("LONGITUDE", style: TextStyle(color: Colors.blue)),
                                                        Text(bikePosition.longitude.toStringAsFixed(4), style: const TextStyle(color: Colors.white)),
                                                    ],
                                                ),
                                            ],
                                        )
                                    ],
                                ),
                            ),
                        ),
                    ),
                ],
            ),
        );
    }

    @override
    void dispose() {
        channel?.sink.close();
        _player.dispose();
        super.dispose();
    }
}
