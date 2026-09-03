import 'package:flutter_test/flutter_test.dart';
import 'package:audio_relay_flutter/main.dart';

void main() {
  testWidgets('AudioRelayApp smoke test', (WidgetTester tester) async {
    await tester.pumpWidget(const AudioRelayApp());
    expect(find.text('Audio Relay'), findsOneWidget);
  });
}
