package scanner

import (
	"context"
	"fmt"
	"net"
	"strings"
	"time"

	"golang.org/x/net/dns/dnsmessage"
	"golang.org/x/net/ipv4"
)

// Resolve queries mDNS on active IPv4 interfaces and races the system resolver.
// Addresses are deliberately not persisted as connection targets or cached.
func Resolve(ctx context.Context, host string) (string, error) {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	addresses := make(chan string, 2)
	go func() {
		ips, err := net.DefaultResolver.LookupIPAddr(ctx, host)
		if err == nil {
			for _, ip := range ips {
				if ip.IP.To4() != nil {
					select {
					case addresses <- ip.IP.String():
					case <-ctx.Done():
					}
					return
				}
			}
		}
	}()
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{})
	if err == nil {
		defer conn.Close()
		name, nameErr := dnsmessage.NewName(host + ".")
		if nameErr != nil {
			return "", nameErr
		}
		packet, packErr := (&dnsmessage.Message{
			Questions: []dnsmessage.Question{{Name: name, Type: dnsmessage.TypeA, Class: dnsmessage.ClassINET}},
		}).Pack()
		if packErr != nil {
			return "", packErr
		}
		go func() {
			buf := make([]byte, 9000)
			for {
				if ctx.Err() != nil {
					return
				}
				_ = conn.SetReadDeadline(time.Now().Add(50 * time.Millisecond))
				n, _, readErr := conn.ReadFromUDP(buf)
				if readErr != nil {
					continue
				}
				var msg dnsmessage.Message
				if msg.Unpack(buf[:n]) != nil || !msg.Header.Response {
					continue
				}
				for _, rr := range append(msg.Answers, msg.Additionals...) {
					if strings.EqualFold(rr.Header.Name.String(), name.String()) && rr.Header.TTL > 0 {
						if a, ok := rr.Body.(*dnsmessage.AResource); ok {
							select {
							case addresses <- net.IP(a.A[:]).String():
							case <-ctx.Done():
							}
							return
						}
					}
				}
			}
		}()
		pc := ipv4.NewPacketConn(conn)
		_ = pc.SetMulticastTTL(255)
		dst := &net.UDPAddr{IP: net.IPv4(224, 0, 0, 251), Port: 5353}
		ifaces, _ := net.Interfaces()
		for _, iface := range ifaces {
			if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagMulticast == 0 || iface.Flags&net.FlagLoopback != 0 {
				continue
			}
			if pc.SetMulticastInterface(&iface) == nil {
				_, _ = pc.WriteTo(packet, nil, dst)
			}
		}
		_, _ = conn.WriteToUDP(packet, dst)
	}
	select {
	case ip := <-addresses:
		return ip, nil
	case <-ctx.Done():
		return "", fmt.Errorf("resolve %s: %w", host, ctx.Err())
	}
}
