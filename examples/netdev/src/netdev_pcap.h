#ifndef NETDEV_PCAP_H
#define NETDEV_PCAP_H

ORYX_DECLARE(int netdev_pcap_open(dev_handler_t **handler,
									char *devname, int flags));


#endif

