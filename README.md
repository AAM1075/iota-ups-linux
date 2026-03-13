# iota-ups-linux
Exposes the LattePanda IOTA's UPS board as a standard battery so it can be used without any special configuration

Normally, Linux will recognize the UPS board as a standard UPS, but in a lot of cases, it might be useful to have it appear to applications (such as a UPower) as a normal battery.

Some applications also poll slowly for UPS systems; this project provides a way to get much faster charging/discharging and battery percentage updates (the UPS board reports its status every second).

## Known Issues
V1.0 of the UPS board does not report battery capacity, most applications will not correctly estimate time remaining and will read the battery health as 0%.
