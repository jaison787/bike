import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';
import 'package:flutter/material.dart';
import 'package:flutter_bluetooth_serial/flutter_bluetooth_serial.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:url_launcher/url_launcher.dart';
import 'package:http/http.dart' as http;
import 'package:flutter_map/flutter_map.dart';
import 'package:latlong2/latlong.dart';
import 'package:web_socket_channel/web_socket_channel.dart';


// =====================================================
// CONFIG - Change this to your Django server URL
// =====================================================
const String kServerBaseUrl = 'http://7fqnrtr5-8000.inc1.devtunnels.ms';
const String kWebSocketUrl = 'ws://7fqnrtr5-8000.inc1.devtunnels.ms/ws/bike/';
const String kBikeId = 'BIKE_001';

void main() {
  runApp(const SmartBikeApp());
}

// =====================================================
// APP ROOT
// =====================================================
class SmartBikeApp extends StatelessWidget {
  const SmartBikeApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      title: 'Smart Bike Safety',
      theme: ThemeData(
        useMaterial3: true,
        brightness: Brightness.dark,
        colorSchemeSeed: const Color(0xFF00D4FF),
        scaffoldBackgroundColor: const Color(0xFF0A0E21),
        appBarTheme: const AppBarTheme(
          backgroundColor: Color(0xFF0A0E21),
          elevation: 0,
          centerTitle: true,
        ),
        cardTheme: CardThemeData(
          color: const Color(0xFF1C1F33),
          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
          elevation: 4,
        ),
      ),
      home: const MainNavigation(),
    );
  }
}

// =====================================================
// MAIN NAVIGATION (Bottom Nav)
// =====================================================
class MainNavigation extends StatefulWidget {
  const MainNavigation({super.key});

  @override
  State<MainNavigation> createState() => _MainNavigationState();
}

class _MainNavigationState extends State<MainNavigation> {
  int _currentIndex = 0;

  // Shared state across pages
  BluetoothConnection? connection;
  String connectionStatus = "Disconnected";
  bool isConnecting = false;
  bool isBluetoothConnected = false;

  // Emergency Contact
  String savedContactName = "";
  String savedContactNumber = "";
  bool silentMode = false;

  // Sensor Data
  int currentVib = 0;
  int currentFsr = 0;
  double currentFall = 0.0;
  double currentTilt = 0.0;
  double currentLat = 0.0;
  double currentLng = 0.0;
  int bikeStatus = 0;
  int countdown = 0;
  String statusMessage = "Normal";
  Color statusColor = Colors.green;

  // Emergency state
  bool emergencyDialogShown = false;

  String _dataBuffer = "";

  // WebSocket for remote tracking
  WebSocketChannel? wsChannel;
  bool isOnline = false;

  @override
  void initState() {
    super.initState();
    _requestPermissions();
    _loadSavedContact();
    _connectWebSocket();
  }

  Future<void> _requestPermissions() async {
    await [
      Permission.bluetooth,
      Permission.bluetoothConnect,
      Permission.bluetoothScan,
      Permission.location,
    ].request();
  }

  Future<void> _loadSavedContact() async {
    SharedPreferences prefs = await SharedPreferences.getInstance();
    String? name = prefs.getString('contactName');
    String? number = prefs.getString('contactNumber');
    bool? silent = prefs.getBool('silentMode');

    if (name == null || number == null || name.isEmpty || number.isEmpty) {
      if (!mounted) return;
      _showContactDialog(context);
    } else {
      setState(() {
        savedContactName = name;
        savedContactNumber = number;
        silentMode = silent ?? false;
      });
    }
  }

  // ==================== BLUETOOTH ====================
  Future<void> connectToBike() async {
    setState(() {
      isConnecting = true;
      connectionStatus = "Searching for SmartBike_System...";
    });

    try {
      List<BluetoothDevice> pairedDevices =
          await FlutterBluetoothSerial.instance.getBondedDevices();
      BluetoothDevice? bikeDevice;

      for (var device in pairedDevices) {
        if (device.name == "SmartBike_System") {
          bikeDevice = device;
          break;
        }
      }

      if (bikeDevice == null) {
        setState(() {
          connectionStatus = "SmartBike_System not found. Please pair it first!";
          isConnecting = false;
        });
        return;
      }

      connection = await BluetoothConnection.toAddress(bikeDevice.address);
      setState(() {
        connectionStatus = "Connected to Smart Bike!";
        isConnecting = false;
        isBluetoothConnected = true;
      });

      // Send saved emergency number to ESP32
      if (savedContactNumber.isNotEmpty) {
        _sendToESP32("PHONE:$savedContactNumber");
      }
      if (savedContactName.isNotEmpty) {
        _sendToESP32("NAME:$savedContactName");
      }
      if (silentMode) {
        _sendToESP32("SILENT:ON");
      }

      // Listen for data from ESP32
      connection!.input!.listen((Uint8List data) {
        _dataBuffer += ascii.decode(data);
        if (_dataBuffer.contains('\n')) {
          List<String> packets = _dataBuffer.split('\n');
          _dataBuffer = packets.removeLast();
          for (String packet in packets) {
            if (packet.trim().isNotEmpty) _parseBluetoothData(packet);
          }
        }
      }).onDone(() {
        setState(() {
          connectionStatus = "Disconnected from hardware.";
          isBluetoothConnected = false;
        });
      });
    } catch (e) {
      setState(() {
        connectionStatus = "Connection failed: $e";
        isConnecting = false;
      });
    }
  }

  void _sendToESP32(String command) {
    if (connection != null && connection!.isConnected) {
      connection!.output.add(ascii.encode("$command\n"));
    }
  }

  void _parseBluetoothData(String jsonString) {
    try {
      var data = jsonDecode(jsonString.trim());
      setState(() {
        if (data.containsKey('vib')) currentVib = data['vib'];
        if (data.containsKey('fsr')) currentFsr = data['fsr'];
        if (data.containsKey('fall')) currentFall = (data['fall'] as num).toDouble();
        if (data.containsKey('tilt')) currentTilt = (data['tilt'] as num).toDouble();
        if (data.containsKey('lat')) currentLat = (data['lat'] as num).toDouble();
        if (data.containsKey('lng')) currentLng = (data['lng'] as num).toDouble();

        if (data.containsKey('status')) {
          bikeStatus = data['status'];
          if (data.containsKey('countdown')) {
            countdown = data['countdown'];
          }
          _updateStatusDisplay();
        }
      });
    } catch (e) {
      // Ignore parse errors
    }
  }

  void _updateStatusDisplay() {
    switch (bikeStatus) {
      case 0:
        statusMessage = "All Systems Normal";
        statusColor = Colors.green;
        emergencyDialogShown = false;
        break;
      case 1:
        statusMessage = "⚠️ Parked Bike Hit!";
        statusColor = Colors.orange;
        break;
      case 2:
        statusMessage = "⚠️ Minor Collision Detected!";
        statusColor = Colors.orange;
        break;
      case 3:
        statusMessage = "🚨 CRASH DETECTED!\nAlerting $savedContactName in ${countdown}s";
        statusColor = Colors.red;
        if (!emergencyDialogShown) {
          emergencyDialogShown = true;
          _showEmergencyDialog();
        }
        break;
      case 4:
        statusMessage = "🚨 SOS SENT to $savedContactName!";
        statusColor = Colors.redAccent;
        break;
    }
  }

  // ==================== EMERGENCY DIALOG ====================
  void _showEmergencyDialog() {
    showDialog(
      context: context,
      barrierDismissible: false,
      builder: (ctx) {
        return AlertDialog(
          backgroundColor: const Color(0xFF2D0000),
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(20),
            side: const BorderSide(color: Colors.red, width: 2),
          ),
          title: const Column(
            children: [
              Icon(Icons.warning_amber_rounded, color: Colors.red, size: 60),
              SizedBox(height: 10),
              Text(
                "CRASH DETECTED!",
                style: TextStyle(
                  color: Colors.red,
                  fontSize: 24,
                  fontWeight: FontWeight.bold,
                ),
                textAlign: TextAlign.center,
              ),
            ],
          ),
          content: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              const Text(
                "An impact has been detected.\nAn emergency alert will be sent if you don't respond.",
                textAlign: TextAlign.center,
                style: TextStyle(color: Colors.white70, fontSize: 16),
              ),
              const SizedBox(height: 20),
              Text(
                "Press below if you're safe!",
                style: TextStyle(
                  color: Colors.yellow.shade300,
                  fontSize: 14,
                  fontWeight: FontWeight.bold,
                ),
                textAlign: TextAlign.center,
              ),
            ],
          ),
          actions: [
            SizedBox(
              width: double.infinity,
              child: ElevatedButton(
                style: ElevatedButton.styleFrom(
                  backgroundColor: Colors.green.shade700,
                  padding: const EdgeInsets.symmetric(vertical: 16),
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(12),
                  ),
                ),
                onPressed: () {
                  // Send cancel to ESP32 via Bluetooth
                  _sendToESP32("CANCEL");
                  // Also cancel via Django API
                  _cancelEmergencyOnServer();
                  Navigator.pop(ctx);
                  setState(() {
                    bikeStatus = 0;
                    statusMessage = "All Systems Normal";
                    statusColor = Colors.green;
                    emergencyDialogShown = false;
                  });
                },
                child: const Text(
                  "✅  I'M OKAY",
                  style: TextStyle(fontSize: 20, fontWeight: FontWeight.bold, color: Colors.white),
                ),
              ),
            ),
          ],
        );
      },
    );
  }

  // ==================== DJANGO API CALLS ====================
  Future<void> _syncConfigToServer() async {
    try {
      await http.post(
        Uri.parse('$kServerBaseUrl/api/update_config/'),
        headers: {'Content-Type': 'application/json'},
        body: jsonEncode({
          'bike_id': kBikeId,
          'emergency_number': savedContactNumber,
          'emergency_name': savedContactName,
          'silent_mode': silentMode,
        }),
      );
    } catch (e) {
      debugPrint('Config sync error: $e');
    }
  }

  Future<void> _cancelEmergencyOnServer() async {
    try {
      await http.post(
        Uri.parse('$kServerBaseUrl/api/cancel_emergency/'),
        headers: {'Content-Type': 'application/json'},
        body: jsonEncode({'bike_id': kBikeId}),
      );
    } catch (e) {
      debugPrint('Cancel error: $e');
    }
  }

  Future<Map<String, dynamic>?> _fetchLatestStatus() async {
    try {
      final response = await http.get(
        Uri.parse('$kServerBaseUrl/api/latest_status/?bike_id=$kBikeId'),
      );
      if (response.statusCode == 200) {
        return jsonDecode(response.body);
      }
    } catch (e) {
      debugPrint('Fetch status error: $e');
    }
    return null;
  }

  Future<List<dynamic>> _fetchAccidentHistory() async {
    try {
      final response = await http.get(
        Uri.parse('$kServerBaseUrl/api/accident_history/?bike_id=$kBikeId'),
      );
      if (response.statusCode == 200) {
        return jsonDecode(response.body);
      }
    } catch (e) {
      debugPrint('History error: $e');
    }
    return [];
  }

  // ==================== WEBSOCKET (Remote) ====================
  void _connectWebSocket() {
    try {
      wsChannel = WebSocketChannel.connect(
        Uri.parse('$kWebSocketUrl$kBikeId/'),
      );
      wsChannel!.stream.listen(
        (message) {
          final data = jsonDecode(message);
          setState(() {
            if (data.containsKey('latitude')) {
              currentLat = double.tryParse(data['latitude'].toString()) ?? 0;
            }
            if (data.containsKey('longitude')) {
              currentLng = double.tryParse(data['longitude'].toString()) ?? 0;
            }
            if (data.containsKey('is_crashed') && data['is_crashed'] == true) {
              if (!emergencyDialogShown) {
                bikeStatus = 3;
                _updateStatusDisplay();
              }
            }
            if (data.containsKey('cancelled') && data['cancelled'] == true) {
              bikeStatus = 0;
              _updateStatusDisplay();
            }
          });
        },
        onError: (e) => debugPrint('WS Error: $e'),
        onDone: () => debugPrint('WS Closed'),
      );
    } catch (e) {
      debugPrint('WS connect error: $e');
    }
  }

  // ==================== CONTACT DIALOG ====================
  void _showContactDialog(BuildContext ctx) {
    TextEditingController nameCtrl =
        TextEditingController(text: savedContactName);
    TextEditingController numberCtrl =
        TextEditingController(text: savedContactNumber);

    showDialog(
      context: ctx,
      barrierDismissible: savedContactName.isNotEmpty,
      builder: (context) {
        return AlertDialog(
          backgroundColor: const Color(0xFF1C1F33),
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(20),
          ),
          title: const Row(
            children: [
              Icon(Icons.emergency, color: Colors.redAccent),
              SizedBox(width: 10),
              Text("Emergency Setup", style: TextStyle(color: Colors.white)),
            ],
          ),
          content: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              const Text(
                "Enter the contact for accident alerts.",
                style: TextStyle(fontSize: 14, color: Colors.grey),
              ),
              const SizedBox(height: 15),
              TextField(
                controller: nameCtrl,
                style: const TextStyle(color: Colors.white),
                decoration: InputDecoration(
                  labelText: "Contact Name",
                  prefixIcon: const Icon(Icons.person),
                  border: OutlineInputBorder(
                    borderRadius: BorderRadius.circular(12),
                  ),
                ),
              ),
              const SizedBox(height: 12),
              TextField(
                controller: numberCtrl,
                keyboardType: TextInputType.phone,
                style: const TextStyle(color: Colors.white),
                decoration: InputDecoration(
                  labelText: "Phone Number (+91...)",
                  prefixIcon: const Icon(Icons.phone),
                  border: OutlineInputBorder(
                    borderRadius: BorderRadius.circular(12),
                  ),
                ),
              ),
            ],
          ),
          actions: [
            SizedBox(
              width: double.infinity,
              child: ElevatedButton(
                style: ElevatedButton.styleFrom(
                  backgroundColor: const Color(0xFF00D4FF),
                  padding: const EdgeInsets.symmetric(vertical: 14),
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(12),
                  ),
                ),
                onPressed: () async {
                    final navigator = Navigator.of(context);
                    
                    if (nameCtrl.text.isNotEmpty && numberCtrl.text.isNotEmpty) {
                      SharedPreferences prefs =
                          await SharedPreferences.getInstance();
                      await prefs.setString('contactName', nameCtrl.text);
                      await prefs.setString('contactNumber', numberCtrl.text);

                      setState(() {
                        savedContactName = nameCtrl.text;
                        savedContactNumber = numberCtrl.text;
                      });

                      // Sync to ESP32 via Bluetooth
                      _sendToESP32("PHONE:${numberCtrl.text}");
                      _sendToESP32("NAME:${nameCtrl.text}");

                      // Sync to Django cloud
                      _syncConfigToServer();

                      if (mounted) navigator.pop();
                    }
                },
                child: const Text(
                  "Save Contact",
                  style: TextStyle(
                    fontSize: 16,
                    fontWeight: FontWeight.bold,
                    color: Colors.black,
                  ),
                ),
              ),
            ),
          ],
        );
      },
    );
  }

  @override
  void dispose() {
    connection?.dispose();
    wsChannel?.sink.close();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final pages = [
      DashboardPage(
        connectionStatus: connectionStatus,
        isConnecting: isConnecting,
        isBluetoothConnected: isBluetoothConnected,
        statusMessage: statusMessage,
        statusColor: statusColor,
        currentVib: currentVib,
        currentFsr: currentFsr,
        currentFall: currentFall,
        currentTilt: currentTilt,
        currentLat: currentLat,
        currentLng: currentLng,
        savedContactName: savedContactName,
        savedContactNumber: savedContactNumber,
        silentMode: silentMode,
        bikeStatus: bikeStatus,
        onConnect: connectToBike,
        onEditContact: () => _showContactDialog(context),
        onToggleSilent: () {
          setState(() => silentMode = !silentMode);
          _sendToESP32(silentMode ? "SILENT:ON" : "SILENT:OFF");
          SharedPreferences.getInstance().then((prefs) {
            prefs.setBool('silentMode', silentMode);
          });
          _syncConfigToServer();
        },
      ),
      LiveMapPage(
        currentLat: currentLat,
        currentLng: currentLng,
        bikeStatus: bikeStatus,
        isBluetoothConnected: isBluetoothConnected,
        onRefresh: () async {
          final data = await _fetchLatestStatus();
          if (data != null && data['latest_status'] != null) {
            final s = data['latest_status'];
            setState(() {
              currentLat = double.tryParse(s['latitude'].toString()) ?? currentLat;
              currentLng = double.tryParse(s['longitude'].toString()) ?? currentLng;
              isOnline = data['is_online'] ?? false;
            });
          }
        },
      ),
      AccidentHistoryPage(
        onFetchHistory: _fetchAccidentHistory,
      ),
    ];

    return Scaffold(
      body: pages[_currentIndex],
      bottomNavigationBar: NavigationBar(
        selectedIndex: _currentIndex,
        onDestinationSelected: (i) => setState(() => _currentIndex = i),
        backgroundColor: const Color(0xFF0D1025),
        indicatorColor: const Color(0xFF00D4FF).withValues(alpha: 0.2),
        destinations: const [
          NavigationDestination(
            icon: Icon(Icons.dashboard_outlined),
            selectedIcon: Icon(Icons.dashboard, color: Color(0xFF00D4FF)),
            label: 'Dashboard',
          ),
          NavigationDestination(
            icon: Icon(Icons.map_outlined),
            selectedIcon: Icon(Icons.map, color: Color(0xFF00D4FF)),
            label: 'Live Map',
          ),
          NavigationDestination(
            icon: Icon(Icons.history_outlined),
            selectedIcon: Icon(Icons.history, color: Color(0xFF00D4FF)),
            label: 'History',
          ),
        ],
      ),
    );
  }
}

// =====================================================
// PAGE 1: DASHBOARD
// =====================================================
class DashboardPage extends StatelessWidget {
  final String connectionStatus;
  final bool isConnecting;
  final bool isBluetoothConnected;
  final String statusMessage;
  final Color statusColor;
  final int currentVib, currentFsr;
  final double currentFall, currentTilt, currentLat, currentLng;
  final String savedContactName, savedContactNumber;
  final bool silentMode;
  final int bikeStatus;
  final VoidCallback onConnect;
  final VoidCallback onEditContact;
  final VoidCallback onToggleSilent;

  const DashboardPage({
    super.key,
    required this.connectionStatus,
    required this.isConnecting,
    required this.isBluetoothConnected,
    required this.statusMessage,
    required this.statusColor,
    required this.currentVib,
    required this.currentFsr,
    required this.currentFall,
    required this.currentTilt,
    required this.currentLat,
    required this.currentLng,
    required this.savedContactName,
    required this.savedContactNumber,
    required this.silentMode,
    required this.bikeStatus,
    required this.onConnect,
    required this.onEditContact,
    required this.onToggleSilent,
  });

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text(
          'SMART BIKE',
          style: TextStyle(
            fontWeight: FontWeight.bold,
            fontSize: 22,
            letterSpacing: 2,
          ),
        ),
        actions: [
          // Silent Mode Toggle
          IconButton(
            icon: Icon(
              silentMode ? Icons.volume_off : Icons.volume_up,
              color: silentMode ? Colors.orange : Colors.white,
            ),
            onPressed: onToggleSilent,
            tooltip: silentMode ? 'Silent Mode ON' : 'Silent Mode OFF',
          ),
          // Settings
          IconButton(
            icon: const Icon(Icons.settings),
            onPressed: onEditContact,
          ),
        ],
      ),
      body: SingleChildScrollView(
        padding: const EdgeInsets.all(16),
        child: Column(
          children: [
            // --- Connection Status ---
            _buildConnectionCard(),
            const SizedBox(height: 12),

            // --- Emergency Contact ---
            if (savedContactName.isNotEmpty) _buildContactCard(),
            const SizedBox(height: 12),

            // --- Status Banner ---
            _buildStatusBanner(),
            const SizedBox(height: 16),

            // --- Sensor Grid ---
            _buildSensorGrid(),
            const SizedBox(height: 16),

            // --- GPS Info ---
            _buildGPSCard(),
            const SizedBox(height: 16),

            // --- Connect Button ---
            if (!isBluetoothConnected) _buildConnectButton(),
          ],
        ),
      ),
    );
  }

  Widget _buildConnectionCard() {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
      decoration: BoxDecoration(
        color: isBluetoothConnected
            ? Colors.green.withValues(alpha: 0.1)
            : Colors.red.withValues(alpha: 0.1),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(
          color: isBluetoothConnected ? Colors.green : Colors.red.shade300,
          width: 1,
        ),
      ),
      child: Row(
        children: [
          Icon(
            isBluetoothConnected ? Icons.bluetooth_connected : Icons.bluetooth_disabled,
            color: isBluetoothConnected ? Colors.green : Colors.red.shade300,
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Text(
              connectionStatus,
              style: TextStyle(
                color: isBluetoothConnected ? Colors.green : Colors.red.shade300,
                fontSize: 14,
              ),
            ),
          ),
          if (isBluetoothConnected)
            Container(
              width: 10,
              height: 10,
              decoration: const BoxDecoration(
                color: Colors.green,
                shape: BoxShape.circle,
              ),
            ),
        ],
      ),
    );
  }

  Widget _buildContactCard() {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(14),
        child: Row(
          children: [
            CircleAvatar(
              backgroundColor: Colors.redAccent.withValues(alpha: 0.2),
              child: const Icon(Icons.emergency, color: Colors.redAccent),
            ),
            const SizedBox(width: 14),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text("Emergency Contact",
                      style: TextStyle(color: Colors.grey, fontSize: 12)),
                  Text(savedContactName,
                      style: const TextStyle(
                          fontWeight: FontWeight.bold, fontSize: 16)),
                  Text(savedContactNumber,
                      style:
                          const TextStyle(color: Colors.grey, fontSize: 13)),
                ],
              ),
            ),
            IconButton(
              icon: const Icon(Icons.edit, color: Colors.grey),
              onPressed: onEditContact,
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildStatusBanner() {
    return AnimatedContainer(
      duration: const Duration(milliseconds: 500),
      padding: const EdgeInsets.all(20),
      decoration: BoxDecoration(
        gradient: LinearGradient(
          colors: [
            statusColor.withValues(alpha: 0.3),
            statusColor.withValues(alpha: 0.1),
          ],
        ),
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: statusColor, width: 2),
      ),
      child: Row(
        children: [
          Icon(
            bikeStatus == 0
                ? Icons.check_circle
                : bikeStatus <= 2
                    ? Icons.warning_amber
                    : Icons.error,
            color: statusColor,
            size: 40,
          ),
          const SizedBox(width: 16),
          Expanded(
            child: Text(
              statusMessage,
              style: TextStyle(
                fontSize: 18,
                fontWeight: FontWeight.bold,
                color: statusColor,
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildSensorGrid() {
    return Row(
      children: [
        Expanded(child: _sensorTile("Vibration", currentVib.toString(), Icons.vibration, Colors.cyan)),
        const SizedBox(width: 8),
        Expanded(child: _sensorTile("FSR", currentFsr.toString(), Icons.touch_app, Colors.purple)),
        const SizedBox(width: 8),
        Expanded(child: _sensorTile("Impact", currentFall.toStringAsFixed(0), Icons.bolt, Colors.orange)),
        const SizedBox(width: 8),
        Expanded(child: _sensorTile("Tilt", "${currentTilt.toStringAsFixed(1)}°", Icons.screen_rotation, Colors.teal)),
      ],
    );
  }

  Widget _sensorTile(String label, String value, IconData icon, Color color) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.symmetric(vertical: 16, horizontal: 8),
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Icon(icon, color: color, size: 24),
            const SizedBox(height: 8),
            Text(value,
                style: const TextStyle(
                    fontSize: 20, fontWeight: FontWeight.bold)),
            const SizedBox(height: 4),
            Text(label,
                style: const TextStyle(fontSize: 11, color: Colors.grey)),
          ],
        ),
      ),
    );
  }

  Widget _buildGPSCard() {
    bool hasGPS = currentLat != 0.0 || currentLng != 0.0;
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          children: [
            Row(
              children: [
                Icon(Icons.gps_fixed,
                    color: hasGPS ? Colors.green : Colors.grey),
                const SizedBox(width: 10),
                Text(
                  hasGPS ? "GPS Signal Active" : "Waiting for GPS Signal...",
                  style: TextStyle(
                    color: hasGPS ? Colors.green : Colors.grey,
                    fontWeight: FontWeight.bold,
                  ),
                ),
              ],
            ),
            if (hasGPS) ...[
              const SizedBox(height: 10),
              Row(
                mainAxisAlignment: MainAxisAlignment.spaceAround,
                children: [
                  Column(
                    children: [
                      const Text("LATITUDE",
                          style: TextStyle(color: Colors.grey, fontSize: 11)),
                      Text(currentLat.toStringAsFixed(6),
                          style: const TextStyle(
                              fontSize: 16, fontWeight: FontWeight.bold)),
                    ],
                  ),
                  Column(
                    children: [
                      const Text("LONGITUDE",
                          style: TextStyle(color: Colors.grey, fontSize: 11)),
                      Text(currentLng.toStringAsFixed(6),
                          style: const TextStyle(
                              fontSize: 16, fontWeight: FontWeight.bold)),
                    ],
                  ),
                ],
              ),
              const SizedBox(height: 10),
              SizedBox(
                width: double.infinity,
                child: OutlinedButton.icon(
                  onPressed: () async {
                    final url = Uri.parse(
                        "https://maps.google.com/?q=$currentLat,$currentLng");
                    if (!await launchUrl(url)) {
                      // Could not open maps
                    }
                  },
                  icon: const Icon(Icons.open_in_new, size: 18),
                  label: const Text("Open in Google Maps"),
                ),
              ),
            ],
          ],
        ),
      ),
    );
  }

  Widget _buildConnectButton() {
    return SizedBox(
      width: double.infinity,
      child: ElevatedButton.icon(
        style: ElevatedButton.styleFrom(
          backgroundColor: const Color(0xFF00D4FF),
          foregroundColor: Colors.black,
          padding: const EdgeInsets.symmetric(vertical: 16),
          shape:
              RoundedRectangleBorder(borderRadius: BorderRadius.circular(14)),
        ),
        onPressed: isConnecting ? null : onConnect,
        icon: isConnecting
            ? const SizedBox(
                width: 20,
                height: 20,
                child: CircularProgressIndicator(
                    strokeWidth: 2, color: Colors.black),
              )
            : const Icon(Icons.bluetooth_searching),
        label: Text(
          isConnecting ? "Connecting..." : "Connect to Bike",
          style: const TextStyle(fontSize: 18, fontWeight: FontWeight.bold),
        ),
      ),
    );
  }
}

// =====================================================
// PAGE 2: LIVE MAP
// =====================================================
class LiveMapPage extends StatelessWidget {
  final double currentLat, currentLng;
  final int bikeStatus;
  final bool isBluetoothConnected;
  final Future<void> Function() onRefresh;

  const LiveMapPage({
    super.key,
    required this.currentLat,
    required this.currentLng,
    required this.bikeStatus,
    required this.isBluetoothConnected,
    required this.onRefresh,
  });

  @override
  Widget build(BuildContext context) {
    bool hasGPS = currentLat != 0.0 || currentLng != 0.0;

    return Scaffold(
      appBar: AppBar(
        title: const Text('Live Bike Location',
            style: TextStyle(fontWeight: FontWeight.bold)),
        actions: [
          IconButton(
            icon: const Icon(Icons.refresh),
            onPressed: onRefresh,
            tooltip: 'Refresh from server',
          ),
        ],
      ),
      body: hasGPS
          ? Stack(
              children: [
                FlutterMap(
                  options: MapOptions(
                    initialCenter: LatLng(currentLat, currentLng),
                    initialZoom: 16,
                  ),
                  children: [
                    TileLayer(
                      urlTemplate:
                          'https://tile.openstreetmap.org/{z}/{x}/{y}.png',
                      userAgentPackageName: 'com.smartbike.app',
                    ),
                    MarkerLayer(
                      markers: [
                        Marker(
                          point: LatLng(currentLat, currentLng),
                          width: 50,
                          height: 50,
                          child: Icon(
                            Icons.two_wheeler,
                            color: bikeStatus > 2 ? Colors.red : Colors.cyan,
                            size: 40,
                          ),
                        ),
                      ],
                    ),
                  ],
                ),
                // Info overlay
                Positioned(
                  bottom: 20,
                  left: 16,
                  right: 16,
                  child: Card(
                    color: const Color(0xFF1C1F33).withValues(alpha: 0.95),
                    child: Padding(
                      padding: const EdgeInsets.all(16),
                      child: Column(
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          Row(
                            children: [
                              Icon(
                                Icons.two_wheeler,
                                color: bikeStatus > 2
                                    ? Colors.red
                                    : Colors.cyan,
                              ),
                              const SizedBox(width: 10),
                              Text(
                                "Bike: $kBikeId",
                                style: const TextStyle(
                                  fontWeight: FontWeight.bold,
                                  fontSize: 16,
                                ),
                              ),
                              const Spacer(),
                              Container(
                                padding: const EdgeInsets.symmetric(
                                    horizontal: 10, vertical: 4),
                                decoration: BoxDecoration(
                                  color: isBluetoothConnected
                                      ? Colors.green.withValues(alpha: 0.2)
                                      : Colors.orange.withValues(alpha: 0.2),
                                  borderRadius: BorderRadius.circular(8),
                                ),
                                child: Text(
                                  isBluetoothConnected ? "BLE" : "GPRS",
                                  style: TextStyle(
                                    color: isBluetoothConnected
                                        ? Colors.green
                                        : Colors.orange,
                                    fontWeight: FontWeight.bold,
                                    fontSize: 12,
                                  ),
                                ),
                              ),
                            ],
                          ),
                          const Divider(color: Colors.grey),
                          Row(
                            mainAxisAlignment:
                                MainAxisAlignment.spaceAround,
                            children: [
                              Column(
                                children: [
                                  const Text("LAT",
                                      style: TextStyle(
                                          color: Colors.cyan,
                                          fontSize: 12)),
                                  Text(
                                    currentLat.toStringAsFixed(6),
                                    style: const TextStyle(
                                        fontWeight: FontWeight.bold),
                                  ),
                                ],
                              ),
                              Column(
                                children: [
                                  const Text("LNG",
                                      style: TextStyle(
                                          color: Colors.cyan,
                                          fontSize: 12)),
                                  Text(
                                    currentLng.toStringAsFixed(6),
                                    style: const TextStyle(
                                        fontWeight: FontWeight.bold),
                                  ),
                                ],
                              ),
                            ],
                          ),
                        ],
                      ),
                    ),
                  ),
                ),
              ],
            )
          : const Center(
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  Icon(Icons.gps_off, size: 80, color: Colors.grey),
                  SizedBox(height: 16),
                  Text(
                    "Waiting for GPS Signal...",
                    style: TextStyle(fontSize: 18, color: Colors.grey),
                  ),
                  SizedBox(height: 8),
                  Text(
                    "Connect to your bike or wait for\nGPRS data from the server.",
                    textAlign: TextAlign.center,
                    style: TextStyle(color: Colors.grey),
                  ),
                ],
              ),
            ),
    );
  }
}

// =====================================================
// PAGE 3: ACCIDENT HISTORY
// =====================================================
class AccidentHistoryPage extends StatefulWidget {
  final Future<List<dynamic>> Function() onFetchHistory;

  const AccidentHistoryPage({super.key, required this.onFetchHistory});

  @override
  State<AccidentHistoryPage> createState() => _AccidentHistoryPageState();
}

class _AccidentHistoryPageState extends State<AccidentHistoryPage> {
  List<dynamic> accidents = [];
  bool isLoading = true;

  @override
  void initState() {
    super.initState();
    _loadHistory();
  }

  Future<void> _loadHistory() async {
    setState(() => isLoading = true);
    accidents = await widget.onFetchHistory();
    setState(() => isLoading = false);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Accident History',
            style: TextStyle(fontWeight: FontWeight.bold)),
        actions: [
          IconButton(
            icon: const Icon(Icons.refresh),
            onPressed: _loadHistory,
          ),
        ],
      ),
      body: isLoading
          ? const Center(child: CircularProgressIndicator())
          : accidents.isEmpty
              ? const Center(
                  child: Column(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      Icon(Icons.shield, size: 80, color: Colors.green),
                      SizedBox(height: 16),
                      Text(
                        "No Accidents Recorded",
                        style: TextStyle(fontSize: 18, color: Colors.grey),
                      ),
                      SizedBox(height: 8),
                      Text(
                        "Stay safe on the road!",
                        style: TextStyle(color: Colors.grey),
                      ),
                    ],
                  ),
                )
              : RefreshIndicator(
                  onRefresh: _loadHistory,
                  child: ListView.builder(
                    padding: const EdgeInsets.all(16),
                    itemCount: accidents.length,
                    itemBuilder: (context, index) {
                      final a = accidents[index];
                      return _buildAccidentCard(a);
                    },
                  ),
                ),
    );
  }

  Widget _buildAccidentCard(Map<String, dynamic> accident) {
    final scenario = accident['scenario'] ?? 0;
    final scenarioDisplay =
        accident['scenario_display'] ?? 'Unknown';
    final wasCancelled = accident['was_cancelled'] ?? false;
    final smsSent = accident['sms_sent'] ?? false;
    final callMade = accident['call_made'] ?? false;
    final timestamp = accident['timestamp'] ?? '';
    final lat = accident['latitude'] ?? '0';
    final lng = accident['longitude'] ?? '0';

    Color scenarioColor;
    IconData scenarioIcon;
    switch (scenario) {
      case 1:
        scenarioColor = Colors.orange;
        scenarioIcon = Icons.directions_car;
        break;
      case 2:
        scenarioColor = Colors.amber;
        scenarioIcon = Icons.warning;
        break;
      case 3:
        scenarioColor = Colors.green;
        scenarioIcon = Icons.check_circle;
        break;
      case 4:
        scenarioColor = Colors.red;
        scenarioIcon = Icons.emergency;
        break;
      default:
        scenarioColor = Colors.grey;
        scenarioIcon = Icons.info;
    }

    return Card(
      margin: const EdgeInsets.only(bottom: 12),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                CircleAvatar(
                  backgroundColor: scenarioColor.withValues(alpha: 0.2),
                  child: Icon(scenarioIcon, color: scenarioColor),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        scenarioDisplay,
                        style: TextStyle(
                          fontWeight: FontWeight.bold,
                          color: scenarioColor,
                          fontSize: 15,
                        ),
                      ),
                      Text(
                        timestamp.toString().substring(0, 19).replaceAll('T', ' '),
                        style: const TextStyle(color: Colors.grey, fontSize: 12),
                      ),
                    ],
                  ),
                ),
                if (wasCancelled)
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                    decoration: BoxDecoration(
                      color: Colors.green.withValues(alpha: 0.2),
                      borderRadius: BorderRadius.circular(6),
                    ),
                    child: const Text(
                      "CANCELLED",
                      style: TextStyle(
                        color: Colors.green,
                        fontSize: 10,
                        fontWeight: FontWeight.bold,
                      ),
                    ),
                  ),
              ],
            ),
            const Divider(height: 20),
            Row(
              children: [
                if (smsSent)
                  const Chip(
                    label: Text("SMS Sent", style: TextStyle(fontSize: 11)),
                    avatar: Icon(Icons.sms, size: 16),
                    backgroundColor: Color(0xFF2A2D45),
                  ),
                const SizedBox(width: 8),
                if (callMade)
                  const Chip(
                    label: Text("Call Made", style: TextStyle(fontSize: 11)),
                    avatar: Icon(Icons.call, size: 16),
                    backgroundColor: Color(0xFF2A2D45),
                  ),
                const Spacer(),
                TextButton.icon(
                  onPressed: () async {
                    final url = Uri.parse(
                        "https://maps.google.com/?q=$lat,$lng");
                    await launchUrl(url);
                  },
                  icon: const Icon(Icons.location_on, size: 16),
                  label: const Text("View Location", style: TextStyle(fontSize: 12)),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}