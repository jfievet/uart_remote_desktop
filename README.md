# uart_remote_desktop
Windows remote desktop in pure C with sender/receiver pair communicating over UART. Captures the screen, detects changes regions and streams them as JPEG compressed cell updates for low bandwith, near real time viewing. It includes a build-in PRBS BER link quality test mode. Build entirely with GCC and a simple .bat script to compile.

```
build.bat
```

Compiles `sender.exe` and `receiver.exe` into `bin\` using GCC (no other tools required).

# Launch

## TCP mode (default)

```
receiver.exe --port 5000
sender.exe --host 127.0.0.1 --port 5000
```

## UART mode

```
receiver.exe --uart --com 13 --speed 500000
sender.exe --uart --com 7 --speed 500000
```

## BER (link test) mode — no display window, console stats only

```
receiver.exe --uart --com 13 --speed 500000 --ber
sender.exe --uart --com 7 --speed 500000 --ber
```

> Start the receiver first in every mode — it waits for the sender to connect/open the port.

## Other options

```
sender.exe --help      # full option list (--host, --port, --interval-ms, --tile-size, --tcp/--uart, --com, --speed, --ber)
receiver.exe --help    # full option list (--tcp/--uart, --port, --com, --speed, --ber)
```
