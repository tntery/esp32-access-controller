# ESP32 Access Controller

An IoT-based access control system using an ESP32 microcontroller that integrates Wiegand protocol card readers with network-based authorization.

## Overview

This project implements a smart access controller that:
- **Reads** user/card IDs from a Wiegand protocol card reader
- **Authenticates** users against an external authorization API over network
- **Controls** an electric/magnetic lock based on API authorization response

## Features

- **Wiegand Protocol Support**: Compatible with standard 26/34-bit Wiegand card readers
- **Network Authorization**: Real-time user verification via external API
- **Remote Lock Control**: Activate/deactivate electric lock based on authorization
- **ESP32 Based**: Leverages WiFi connectivity and processing power of ESP32
- **Flexible API Integration**: Configurable authorization endpoint

## System Architecture

```
[Card Reader] -- Wiegand Protocol --> [ESP32] -- HTTP/HTTPS --> [Authorization API]
                                         |
                                         v
                                  [Electric Lock/Maglock]
```

## Hardware Requirements

- **ESP32 Microcontroller**: Main processor
- **Wiegand Card Reader**: 26-bit or 34-bit compatible reader
- **Electric Lock/Maglock**: Solenoid-based door lock
- **Power Supply**: Appropriate PSU for ESP32, lock, and reader
- **Network**: WiFi connectivity for API communication

## Pin Configuration

Configure the following GPIO pins in your code:
- **Wiegand Data0**: GPIO pin for data line 0
- **Wiegand Data1**: GPIO pin for data line 1
- **Lock Control**: GPIO pin for lock relay/solenoid

## Software Setup

### Prerequisites

- [PlatformIO](https://platformio.org/) installed
- ESP32 board support
- Arduino framework compatible libraries

### Installation

1. Clone the repository:
```bash
git clone <repository-url>
cd esp32-access-controller
```

2. Install dependencies:
```bash
pio lib install
```

3. Configure your WiFi and API settings in the project configuration

4. Build and upload:
```bash
pio run -t upload
```

## Configuration

### WiFi Settings
Configure WiFi credentials:
- SSID
- Password

### API Configuration
Set up your authorization API endpoint:
- **Base URL**: Your authorization server address
- **Endpoint**: User authorization endpoint
- **Method**: HTTP POST/GET
- **Authentication**: API key or token if required

### Lock Configuration
- Lock activation time (how long to keep lock open)
- GPIO pin for lock control
- Lock activation state (HIGH/LOW)

## API Integration

### Expected Authorization API

The controller sends a request with the user ID and expects a response:

**Request:**
```json
{
  "user_id": "12345678",
  "timestamp": "2024-03-16T10:30:00Z"
}
```

**Response:**
```json
{
  "authorized": true,
  "user_name": "John Doe",
  "access_level": "standard"
}
```

Access is granted if `authorized` is `true` in the response.

## Usage

1. Power on the ESP32 and ensure it connects to WiFi
2. Present a card to the Wiegand reader
3. The ESP32 reads the card ID and sends it to the authorization API
4. If authorized, the electric lock activates for the configured duration
5. If not authorized, the lock remains disengaged and an error may be logged

## License

[Add your license information here]

## Support

For issues or questions, please create an issue in the repository.

## Contributing

Contributions are welcome! Please submit pull requests with improvements or bug fixes.
