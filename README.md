hmon -- Host Monitor

This is an agent that periodicaly monitors the resource usage of the host it is running on.

Specifically, it monitors CPU, memory/swap, network, disks, and the host's up time.

Network

The agent obtains network stats from /proc/net/dev and calculates the receive/transmit speed for a period of time in bytes/sec.
