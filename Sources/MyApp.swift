import SwiftUI

@main
struct MyApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}

struct ContentView: View {
    @State private var taps = 0

    var body: some View {
        VStack(spacing: 20) {
            Text("Hello from my own app")
                .font(.title2)
                .bold()

            Text("Taps: \(taps)")
                .font(.headline)

            Button("Tap me") {
                taps += 1
            }
            .buttonStyle(.borderedProminent)
        }
        .padding()
    }
}
