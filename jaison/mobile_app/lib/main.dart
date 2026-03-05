import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:geolocator/geolocator.dart';
import 'package:telephony/telephony.dart';
import 'package:url_launcher/url_launcher.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:latlong2/latlong.dart';
import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';
import 'package:flutter_bluetooth_serial/flutter_bluetooth_serial.dart' as classic;

// Replace with your ESP32 UUIDs if known. Otherwise, the app will try to find suitable ones.
final String serviceUuid = "4fafc201-1fb5-459e-8fcc-c5c9c331914b";
final String characteristicUuidRx = "beb5483e-36e1-4688-b7f5-ea07361b26a8"; // App receives from ESP32
final String characteristicUuidTx = "beb5483e-36e1-4688-b7f5-ea07361b26a8"; // App sends to ESP32

void main() => runApp(const SmartBikeApp());

class SmartBikeApp extends StatelessWidget {
  const SmartBikeApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      title: 'Smart Bike App',
      theme: ThemeData.dark().copyWith(
        primaryColor: Colors.blueAccent,
      ),
      home: const BluetoothLoginScreen(),
    );
  }
}

class BluetoothLoginScreen extends StatefulWidget {
  const BluetoothLoginScreen({super.key});

  @override
  State<BluetoothLoginScreen> createState() => _BluetoothLoginScreenState();
}

class _BluetoothLoginScreenState extends State<BluetoothLoginScreen> {
  List<ScanResult> scanResults = [];
  bool isScanning = false;
  late StreamSubscription<List<ScanResult>> _scanResultsSubscription;
  late StreamSubscription<bool> _isScanningSubscription;

  @override
  void initState() {
    super.initState();
    _scanResultsSubscription = FlutterBluePlus.scanResults.listen((results) {
      if (mounted) {
        setState(() {
          scanResults = results;
        });
      }
    });

    _isScanningSubscription = FlutterBluePlus.isScanning.listen((state) {
      if (mounted) {
        setState(() {
          isScanning = state;
        });
      }
    });

    _checkPermissions();
  }

  Future<void> _checkPermissions() async {
    // Basic check. flutter_blue_plus handles permission requests during connection/scan on modern android
    bool locationEnabled = await Geolocator.isLocationServiceEnabled();
    if (!locationEnabled) {
       await Geolocator.requestPermission();
    }
  }

  @override
  void dispose() {
    _scanResultsSubscription.cancel();
    _isScanningSubscription.cancel();
    super.dispose();
  }

  void startScan() async {
    try {
      await FlutterBluePlus.startScan(timeout: const Duration(seconds: 15));
    } catch (e) {
      debugPrint("Error starting scan: $e");
    }
  }

  void stopScan() async {
    await FlutterBluePlus.stopScan();
  }

  void connectToDevice(BluetoothDevice device) async {
    stopScan();
    try {
      await device.connect();
      if (!mounted) return;
      Navigator.pushReplacement(
        context,
        MaterialPageRoute(
          builder: (context) => EmergencyNumberScreen(device: device),
        ),
      );
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Failed to connect: $e')),
        );
      }
    }
  }

  void connectToPairedBike() async {
    try {
      // 1. Get the list of paired devices
      List<classic.BluetoothDevice> pairedDevices = await classic.FlutterBluetoothSerial.instance.getBondedDevices();
      
      // 2. Find the ESP32 by its name
      classic.BluetoothDevice? bikeDevice;
      for (var device in pairedDevices) {
        if (device.name == "SmartBike_System") {
          bikeDevice = device;
          break;
        }
      }

      if (bikeDevice == null) {
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(content: Text("'SmartBike_System' not found in paired devices!")),
          );
        }
        return;
      }

      // 3. Connect to the ESP32
      classic.BluetoothConnection connection = await classic.BluetoothConnection.toAddress(bikeDevice.address);
      debugPrint('Connected to the Smart Bike via Bluetooth Classic!');

      if (!mounted) return;
      
      Navigator.push(
        context,
        MaterialPageRoute(
          builder: (context) => ClassicEmergencyNumberScreen(connection: connection),
        ),
      );

    } catch (e) {
      debugPrint("Cannot connect, exception occurred: $e");
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Failed to connect: $e')),
        );
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Connect to Bike'),
        actions: [
          if (isScanning)
            IconButton(
              icon: const Icon(Icons.stop),
              onPressed: stopScan,
            )
          else
            IconButton(
              icon: const Icon(Icons.search),
              onPressed: startScan,
            )
        ],
      ),
      body: Column(
        children: [
          Padding(
            padding: const EdgeInsets.all(16.0),
            child: ElevatedButton.icon(
              onPressed: connectToPairedBike,
              icon: const Icon(Icons.bluetooth_connected),
              label: const Text("CONNECT TO PAIRED BIKE (SmartBike_System)"),
              style: ElevatedButton.styleFrom(
                minimumSize: const Size(double.infinity, 60),
                backgroundColor: Colors.blueAccent,
                foregroundColor: Colors.white,
              ),
            ),
          ),
          const Divider(height: 1),
          Expanded(
            child: scanResults.isEmpty
                ? const Center(child: Text("Or scan for BLE devices..."))
                : ListView.builder(
                    itemCount: scanResults.length,
                    itemBuilder: (context, index) {
                      final r = scanResults[index];
                      if (r.device.advName.isEmpty && r.device.platformName.isEmpty) return const SizedBox();
                      final name = r.device.advName.isNotEmpty ? r.device.advName : r.device.platformName;
                      return ListTile(
                        title: Text(name),
                        subtitle: Text(r.device.remoteId.str),
                        trailing: ElevatedButton(
                          onPressed: () => connectToDevice(r.device),
                          child: const Text('CONNECT'),
                        ),
                      );
                    },
                  ),
          ),
        ],
      ),
    );
  }
}

class ClassicEmergencyNumberScreen extends StatefulWidget {
  final classic.BluetoothConnection connection;
  const ClassicEmergencyNumberScreen({super.key, required this.connection});

  @override
  State<ClassicEmergencyNumberScreen> createState() => _ClassicEmergencyNumberScreenState();
}

class _ClassicEmergencyNumberScreenState extends State<ClassicEmergencyNumberScreen> {
  final TextEditingController _numberController = TextEditingController();

  void saveEmergencyNumber() {
    if (_numberController.text.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text("Please enter a valid number")));
      return;
    }

    final String emgNumber = _numberController.text.trim();

    Navigator.pushReplacement(
      context,
      MaterialPageRoute(
        builder: (context) => ClassicDashboardScreen(
          connection: widget.connection,
          emergencyNumber: emgNumber,
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Setup Emergency Contact')),
      body: Padding(
        padding: const EdgeInsets.all(16.0),
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            const Text(
              "Setup your emergency contact for the Smart Bike.",
              textAlign: TextAlign.center,
              style: TextStyle(fontSize: 18),
            ),
            const SizedBox(height: 20),
            TextField(
              controller: _numberController,
              keyboardType: TextInputType.phone,
              decoration: const InputDecoration(
                labelText: "Emergency Mobile Number",
                border: OutlineInputBorder(),
                prefixIcon: Icon(Icons.phone),
              ),
            ),
            const SizedBox(height: 20),
            ElevatedButton(
              onPressed: saveEmergencyNumber,
              style: ElevatedButton.styleFrom(minimumSize: const Size(double.infinity, 50)),
              child: const Text('GO TO DASHBOARD'),
            )
          ],
        ),
      ),
    );
  }
}

class ClassicDashboardScreen extends StatefulWidget {
  final classic.BluetoothConnection connection;
  final String emergencyNumber;

  const ClassicDashboardScreen({
    super.key,
    required this.connection,
    required this.emergencyNumber,
  });

  @override
  State<ClassicDashboardScreen> createState() => _ClassicDashboardScreenState();
}

class _ClassicDashboardScreenState extends State<ClassicDashboardScreen> {
  final MapController mapController = MapController();
  LatLng bikePosition = const LatLng(12.9716, 77.5946); // Default location Bangalore
  bool isCrashed = false;
  int vib = 0;
  int fsr = 0;
  double fall = 0.0;
  int status = 0;
  int countdown = 0;
  
  final Telephony telephony = Telephony.instance;
  StreamSubscription<Position>? _positionSubscription;
  String _incomingBuffer = "";

  @override
  void initState() {
    super.initState();
    _initBluetoothListener();
    _initLocation();
    _requestSmsPermissions();
  }

  void _requestSmsPermissions() async {
    bool? permissionsGranted = await telephony.requestPhoneAndSmsPermissions;
    debugPrint("SMS Permissions granted: $permissionsGranted");
  }

  void _initLocation() async {
    bool serviceEnabled = await Geolocator.isLocationServiceEnabled();
    if (!serviceEnabled) return;

    LocationPermission permission = await Geolocator.checkPermission();
    if (permission == LocationPermission.denied) {
      permission = await Geolocator.requestPermission();
      if (permission == LocationPermission.denied) return;
    }

    if (permission == LocationPermission.deniedForever) return;

    Position currentPos = await Geolocator.getCurrentPosition();
    if (mounted) {
      setState(() {
        bikePosition = LatLng(currentPos.latitude, currentPos.longitude);
      });
      mapController.move(bikePosition, 16.0);
    }

    _positionSubscription = Geolocator.getPositionStream().listen((Position position) {
      if (mounted) {
        setState(() {
          bikePosition = LatLng(position.latitude, position.longitude);
        });
      }
    });
  }

  void _initBluetoothListener() {
    widget.connection.input!.listen((Uint8List data) {
      // Decode the raw bytes into a string and add to buffer
      _incomingBuffer += ascii.decode(data);
      
      // Look for complete JSON packets (between {} )
      while (_incomingBuffer.contains('{') && _incomingBuffer.contains('}')) {
        int startIndex = _incomingBuffer.indexOf('{');
        int endIndex = _incomingBuffer.indexOf('}', startIndex);
        
        if (endIndex != -1) {
          String jsonPart = _incomingBuffer.substring(startIndex, endIndex + 1);
          _incomingBuffer = _incomingBuffer.substring(endIndex + 1);
          
          try {
            var bikeData = jsonDecode(jsonPart);
            if (mounted) {
              setState(() {
                vib = bikeData['vib'] ?? vib;
                fsr = bikeData['fsr'] ?? fsr;
                fall = (bikeData['fall'] ?? 0.0) + 0.0;
                status = bikeData['status'] ?? status;
                
                if (status == 3) {
                  countdown = bikeData['countdown'] ?? 0;
                  if (!isCrashed) {
                    _triggerAccidentResponse();
                  }
                } else if (status != 3) {
                    isCrashed = false;
                }
              });
            }
          } catch (e) {
            debugPrint("Error parsing JSON packet: $e");
          }
        } else {
          break;
        }
      }
    }).onDone(() {
      debugPrint('Disconnected by remote request');
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text("Bike Disconnected")));
        Navigator.of(context).popUntil((route) => route.isFirst);
      }
    });
  }

  void _triggerAccidentResponse() async {
    if (isCrashed) return;
    setState(() {
      isCrashed = true;
    });

    String mapsLink = "https://www.google.com/maps/search/?api=1&query=${bikePosition.latitude},${bikePosition.longitude}";
    String smsMessage = "ALERT! A bike crash has been detected. Location: $mapsLink";

    try {
      await telephony.sendSms(to: widget.emergencyNumber, message: smsMessage);
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Emergency SMS Sent!')));
      }
    } catch (e) {
      debugPrint("SMS Error: $e");
    }

    String telUri = "tel:${widget.emergencyNumber}";
    if (await canLaunchUrl(Uri.parse(telUri))) {
      await launchUrl(Uri.parse(telUri));
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text("Live Bike Dashboard"),
        backgroundColor: status == 3 ? Colors.red : (status == 2 ? Colors.orange : Colors.blueAccent),
        actions: [
          Center(child: Padding(
            padding: const EdgeInsets.only(right: 16.0),
            child: Text(status == 3 ? "EMERGENCY" : "ONLINE", style: const TextStyle(fontWeight: FontWeight.bold)),
          ))
        ],
      ),
      body: Stack(
        children: [
          FlutterMap(
            mapController: mapController,
            options: MapOptions(initialCenter: bikePosition, initialZoom: 16),
            children: [
              TileLayer(
                urlTemplate: 'https://tile.openstreetmap.org/{z}/{x}/{y}.png',
                userAgentPackageName: 'com.example.smart_bike_app',
              ),
              MarkerLayer(
                markers: [
                  Marker(
                    point: bikePosition,
                    width: 60,
                    height: 60,
                    child: Column(
                      children: [
                        Container(
                          padding: const EdgeInsets.all(4),
                          decoration: BoxDecoration(color: Colors.white, borderRadius: BorderRadius.circular(10), boxShadow: [const BoxShadow(blurRadius: 5)]),
                          child: const Text("ME", style: TextStyle(color: Colors.black, fontWeight: FontWeight.bold, fontSize: 10)),
                        ),
                        Icon(Icons.directions_bike, color: status == 3 ? Colors.red : Colors.blue, size: 40),
                      ],
                    ),
                  ),
                ],
              ),
            ],
          ),
          
          Positioned(
            top: 20,
            left: 20,
            right: 20,
            child: Row(
              children: [
                Expanded(child: _dataCard("Vibration", "$vib", Icons.vibration, Colors.orange)),
                const SizedBox(width: 10),
                Expanded(child: _dataCard("Pressure (FSR)", "$fsr", Icons.compress, Colors.green)),
              ],
            ),
          ),

          if (status == 3)
            Positioned(
              top: 100,
              left: 20,
              right: 20,
              child: Card(
                color: Colors.red.withOpacity(0.9),
                elevation: 10,
                child: Padding(
                  padding: const EdgeInsets.all(20.0),
                  child: Column(
                    children: [
                      const Icon(Icons.warning, color: Colors.white, size: 60),
                      const SizedBox(height: 10),
                      Text(
                        "ACCIDENT DETECTED!\nCountdown: $countdown",
                        textAlign: TextAlign.center,
                        style: const TextStyle(color: Colors.white, fontSize: 24, fontWeight: FontWeight.bold),
                      ),
                      const SizedBox(height: 10),
                      const Text("Emergency services and contacts are being notified.", textAlign: TextAlign.center, style: TextStyle(color: Colors.white70)),
                      const SizedBox(height: 15),
                      ElevatedButton(
                        onPressed: () => setState(() { status = 1; isCrashed = false; }),
                        style: ElevatedButton.styleFrom(backgroundColor: Colors.white, foregroundColor: Colors.red),
                        child: const Text("I AM SAFE (CANCEL)"),
                      )
                    ],
                  ),
                ),
              ),
            ),

          Positioned(
            bottom: 20,
            left: 20,
            right: 20,
            child: Card(
              color: Colors.black.withOpacity(0.8),
              child: Padding(
                padding: const EdgeInsets.all(16.0),
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Text("Emergency Contact: ${widget.emergencyNumber}", style: const TextStyle(color: Colors.white70)),
                    const SizedBox(height: 10),
                    Row(
                      mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                      children: [
                        _statusIndicator("System", status != 0),
                        _statusIndicator("Bluetooth", true),
                        _statusIndicator("GPS", true),
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

  Widget _dataCard(String title, String value, IconData icon, Color color) {
    return Card(
      color: Colors.black.withOpacity(0.7),
      child: Padding(
        padding: const EdgeInsets.all(12.0),
        child: Column(
          children: [
            Icon(icon, color: color, size: 30),
            const SizedBox(height: 5),
            Text(value, style: const TextStyle(fontSize: 20, fontWeight: FontWeight.bold, color: Colors.white)),
            Text(title, style: const TextStyle(fontSize: 10, color: Colors.white54)),
          ],
        ),
      ),
    );
  }

  Widget _statusIndicator(String label, bool active) {
    return Row(
      children: [
        Icon(Icons.circle, size: 10, color: active ? Colors.green : Colors.red),
        const SizedBox(width: 5),
        Text(label, style: const TextStyle(color: Colors.white, fontSize: 12)),
      ],
    );
  }

  @override
  void dispose() {
    _positionSubscription?.cancel();
    widget.connection.dispose();
    super.dispose();
  }
}

class EmergencyNumberScreen extends StatefulWidget {
  final BluetoothDevice device;
  const EmergencyNumberScreen({super.key, required this.device});

  @override
  State<EmergencyNumberScreen> createState() => _EmergencyNumberScreenState();
}

class _EmergencyNumberScreenState extends State<EmergencyNumberScreen> {
  final TextEditingController _numberController = TextEditingController();
  BluetoothCharacteristic? _writeCharacteristic;
  BluetoothCharacteristic? _notifyCharacteristic;
  bool isDiscovering = true;

  @override
  void initState() {
    super.initState();
    _discoverServices();
  }

  Future<void> _discoverServices() async {
    try {
      List<BluetoothService> services = await widget.device.discoverServices();
      for (var service in services) {
        for (var characteristic in service.characteristics) {
          if (characteristic.properties.write || characteristic.properties.writeWithoutResponse) {
            _writeCharacteristic = characteristic;
          }
          if (characteristic.properties.notify || characteristic.properties.read) {
            _notifyCharacteristic = characteristic;
          }
        }
      }
      setState(() {
        isDiscovering = false;
      });
    } catch (e) {
      debugPrint("Error discovering services: $e");
      setState(() {
        isDiscovering = false;
      });
    }
  }

  void saveEmergencyNumber() async {
    if (_numberController.text.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text("Please enter a valid number")));
      return;
    }

    final String emgNumber = _numberController.text.trim();

    if (_writeCharacteristic != null) {
      try {
        await _writeCharacteristic!.write(utf8.encode(emgNumber), withoutResponse: _writeCharacteristic!.properties.writeWithoutResponse);
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text("Emergency number synced to hardware.")));
          
          Navigator.pushReplacement(
            context,
            MaterialPageRoute(
              builder: (context) => DashboardScreen(
                device: widget.device,
                emergencyNumber: emgNumber,
                notifyCharacteristic: _notifyCharacteristic,
              ),
            ),
          );
        }
      } catch (e) {
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text("Failed to write to device: $e")));
        }
      }
    } else {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text("No writable characteristic found. Proceeding anyway.")));
        Navigator.pushReplacement(
          context,
          MaterialPageRoute(
            builder: (context) => DashboardScreen(
              device: widget.device,
              emergencyNumber: emgNumber,
              notifyCharacteristic: _notifyCharacteristic,
            ),
          ),
        );
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Setup Emergency Contact')),
      body: isDiscovering
          ? const Center(child: CircularProgressIndicator())
          : Padding(
              padding: const EdgeInsets.all(16.0),
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  const Text(
                    "Enter the emergency mobile number. This will be updated on your hardware.",
                    textAlign: TextAlign.center,
                    style: TextStyle(fontSize: 18),
                  ),
                  const SizedBox(height: 20),
                  TextField(
                    controller: _numberController,
                    keyboardType: TextInputType.phone,
                    decoration: const InputDecoration(
                      labelText: "Emergency Mobile Number",
                      border: OutlineInputBorder(),
                      prefixIcon: Icon(Icons.phone),
                    ),
                  ),
                  const SizedBox(height: 20),
                  ElevatedButton(
                    onPressed: saveEmergencyNumber,
                    style: ElevatedButton.styleFrom(minimumSize: const Size(double.infinity, 50)),
                    child: const Text('SAVE & CONTINUE'),
                  )
                ],
              ),
            ),
    );
  }
}

class DashboardScreen extends StatefulWidget {
  final BluetoothDevice device;
  final String emergencyNumber;
  final BluetoothCharacteristic? notifyCharacteristic;

  const DashboardScreen({
    super.key,
    required this.device,
    required this.emergencyNumber,
    this.notifyCharacteristic,
  });

  @override
  State<DashboardScreen> createState() => _DashboardScreenState();
}

class _DashboardScreenState extends State<DashboardScreen> {
  final MapController mapController = MapController();
  LatLng bikePosition = const LatLng(12.9716, 77.5946); // Default location
  bool isCrashed = false;
  late StreamSubscription<BluetoothConnectionState> _connectionStateSubscription;
  StreamSubscription<List<int>>? _notifySubscription;
  final Telephony telephony = Telephony.instance;
  StreamSubscription<Position>? _positionSubscription;

  @override
  void initState() {
    super.initState();
    _initDevice();
    _initLocation();
    _requestSmsPermissions();
  }

  void _requestSmsPermissions() async {
    bool? permissionsGranted = await telephony.requestPhoneAndSmsPermissions;
    debugPrint("SMS Permissions granted: $permissionsGranted");
  }

  void _initLocation() async {
    bool serviceEnabled = await Geolocator.isLocationServiceEnabled();
    if (!serviceEnabled) {
      return;
    }

    LocationPermission permission = await Geolocator.checkPermission();
    if (permission == LocationPermission.denied) {
      permission = await Geolocator.requestPermission();
      if (permission == LocationPermission.denied) {
        return;
      }
    }

    if (permission == LocationPermission.deniedForever) {
      return;
    }

    Position currentPos = await Geolocator.getCurrentPosition();
    if (mounted) {
      setState(() {
        bikePosition = LatLng(currentPos.latitude, currentPos.longitude);
      });
      mapController.move(bikePosition, 16.0);
    }

    _positionSubscription = Geolocator.getPositionStream().listen((Position position) {
      if (mounted) {
        setState(() {
          bikePosition = LatLng(position.latitude, position.longitude);
        });
      }
    });
  }

  void _initDevice() {
    _connectionStateSubscription = widget.device.connectionState.listen((state) {
      if (state == BluetoothConnectionState.disconnected && mounted) {
        showDialog(
          context: context,
          barrierDismissible: false,
          builder: (context) => AlertDialog(
            title: const Text('Device Disconnected'),
            content: const Text('Connection to the bike has been lost.'),
            actions: [
              TextButton(
                onPressed: () {
                  Navigator.of(context).popUntil((route) => route.isFirst);
                },
                child: const Text('OK'),
              ),
            ],
          ),
        );
      }
    });

    if (widget.notifyCharacteristic != null) {
      widget.notifyCharacteristic!.setNotifyValue(true);
      _notifySubscription = widget.notifyCharacteristic!.lastValueStream.listen((value) {
        String msg = utf8.decode(value);
        if (msg.toLowerCase().contains('crash') || msg.toLowerCase().contains('accident') || msg.trim() == "1") {
          _triggerAccidentResponse();
        }
      });
    }
  }

  void _triggerAccidentResponse() async {
    if (isCrashed) return; // Prevent multiple triggers
    setState(() {
      isCrashed = true;
    });

    String mapsLink = "https://www.google.com/maps/search/?api=1&query=${bikePosition.latitude},${bikePosition.longitude}";
    String smsMessage = "ALERT! A bike crash has been detected. Location: $mapsLink";

    // Send SMS
    try {
      await telephony.sendSms(
        to: widget.emergencyNumber,
        message: smsMessage,
      );
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('Emergency SMS Sent!')),
        );
      }
    } catch (e) {
      debugPrint("Failed to send automatic SMS: $e");
      // Fallback to URL launcher if telephony fails
      String uri = 'sms:${widget.emergencyNumber}?body=${Uri.encodeComponent(smsMessage)}';
      if (await canLaunchUrl(Uri.parse(uri))) {
        await launchUrl(Uri.parse(uri));
      }
    }

    // Trigger Phone Call
    String telUri = "tel:${widget.emergencyNumber}";
    if (await canLaunchUrl(Uri.parse(telUri))) {
      await launchUrl(Uri.parse(telUri));
    }
  }

  @override
  void dispose() {
    _connectionStateSubscription.cancel();
    _notifySubscription?.cancel();
    _positionSubscription?.cancel();
    widget.device.disconnect();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text("Bike Dashboard"),
        backgroundColor: isCrashed ? Colors.red : Colors.green[800],
      ),
      body: Stack(
        children: [
          FlutterMap(
            mapController: mapController,
            options: MapOptions(initialCenter: bikePosition, initialZoom: 16),
            children: [
              TileLayer(
                urlTemplate: 'https://tile.openstreetmap.org/{z}/{x}/{y}.png',
                userAgentPackageName: 'com.example.smart_bike_app',
              ),
              MarkerLayer(
                markers: [
                  Marker(
                    point: bikePosition,
                    width: 50,
                    height: 50,
                    child: Icon(
                      Icons.electric_scooter,
                      color: isCrashed ? Colors.red : Colors.blue,
                      size: 45,
                    ),
                  ),
                ],
              ),
            ],
          ),
          if (isCrashed)
            Positioned(
              top: 20,
              left: 20,
              right: 20,
              child: Card(
                color: Colors.red.withValues(alpha: 0.9),
                child: const Padding(
                  padding: EdgeInsets.all(16.0),
                  child: Text(
                    "CRASH DETECTED!\nEmergency protocol activated.\nSMS and Call sent.",
                    textAlign: TextAlign.center,
                    style: TextStyle(color: Colors.white, fontSize: 18, fontWeight: FontWeight.bold),
                  ),
                ),
              ),
            ),
          Positioned(
            bottom: 20,
            left: 20,
            right: 20,
            child: Card(
              color: Colors.black.withValues(alpha: 0.8),
              child: Padding(
                padding: const EdgeInsets.all(16.0),
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Text("Emergency Contact: ${widget.emergencyNumber}", style: const TextStyle(color: Colors.white)),
                    const SizedBox(height: 10),
                    ElevatedButton(
                      style: ElevatedButton.styleFrom(backgroundColor: Colors.red),
                      onPressed: _triggerAccidentResponse, // Testing button
                      child: const Text('TEST CRASH ALERT', style: TextStyle(color: Colors.white)),
                    ),
                  ],
                ),
              ),
            ),
          ),
        ],
      ),
    );
  }
}
