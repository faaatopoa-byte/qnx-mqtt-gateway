# Mosquitto headers (upstream, vendored)

The three headers in this directory are copied verbatim from Eclipse
Mosquitto 2.0.15 (`mosquitto_plugin.h`, `mosquitto_broker.h`,
`mosquitto.h`). They are published under the Eclipse Public License
2.0 OR BSD-3-Clause; the license header at the top of each file is
unchanged.

They are here for one reason: the host build and the QNX build need
the broker plugin API headers, and this repo does not require a
Mosquitto install to compile against. On a QNX target with the
official port installed, the port provides these same headers under
`$QNX_TARGET/usr/local/include`.

These headers are byte-identical to the ones in the official QNX
port of Mosquitto 2.0.15 (`github.com/qnx-ports/mosquitto`,
`qnx_v2.0.15` branch). That match was checked when the headers were
copied. Do not edit them. If Mosquitto moves to a newer version,
replace all three files together and re-check the match.
