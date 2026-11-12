# Paho embedded C protocol sources

Copyright (c) 2014 IBM Corp. and the contributors named in each source header.
Distributed under the Eclipse Public License 1.0 and Eclipse Distribution License 1.0, as stated by the retained upstream headers. The accompanying texts are in `licenses/EPL-1.0.html` and `licenses/EDL-1.0.txt`, downloaded from the official Eclipse license pages.

Source: the Paho protocol/client files in MiCO SDK commit `6c465211d3ff8797cd835e400ec54a06530dd476`, with the Phicomm patch stack pinned in `../lab-shared-source.json`. MiCO platform/runtime sources are not part of this port.

Local changes bind network/timer primitives to OpenBeken, adapt thread/publication primitives, avoid SDK socket/enum macro collisions and retain bounded protocol buffers and deadlines. Exact diffs are in `../lab-openbeken.patch`; file hashes and patch hashes are in `../lab-shared-source.json`.

License sources:
- https://www.eclipse.org/legal/epl/epl-v10.html
- https://www.eclipse.org/org/documents/edl-v10/
