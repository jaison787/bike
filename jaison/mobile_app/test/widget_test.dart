import 'package:flutter_test/flutter_test.dart';
import 'package:smart_bike_app/main.dart';

void main() {
  testWidgets('Bike Dashboard loads', (WidgetTester tester) async {
    // Build our app and trigger a frame.
    await tester.pumpWidget(const BikeDashboard());

    // Verify that our dashboard shows the title.
    expect(find.text('SMART BIKE TRACKER'), findsOneWidget);
  });
}
