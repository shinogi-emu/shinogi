#!/bin/bash
# ISC dhclient callback for Shinogi. /etc lives on the guest ramfs.
# The interface name and commands are fixed; only typed IPv4 lease values
# from dhclient are used. Other DHCP options do not become shell commands.
PATH=/bin
export PATH

[ "$interface" = eth0 ] || exit 1

ipv4() {
    case "$1" in ''|*[!0-9.]*) return 1 ;; esac
    local a b c d extra
    IFS=. read -r a b c d extra <<< "$1"
    [ -z "$extra" ] || return 1
    for part in "$a" "$b" "$c" "$d"; do
        [ -n "$part" ] && [ ${#part} -le 3 ] || return 1
        [ "$((10#$part))" -le 255 ] || return 1
    done
}

remove_router() {
    local router
    for router in $old_routers; do
        ipv4 "$router" && /bin/route delete default "$router" >/dev/null 2>&1
    done
    return 0
}

ip_number() {
    local a b c d
    IFS=. read -r a b c d <<< "$1"
    echo "$(((10#$a << 24) | (10#$b << 16) | (10#$c << 8) | 10#$d))"
}

case "$reason" in
    PREINIT)
        /bin/ifconfig eth0 addr 0.0.0.0 netmask 0.0.0.0 broadaddr 255.255.255.255 up
        ;;
    BOUND|RENEW|REBIND|REBOOT)
        ipv4 "$new_ip_address" && ipv4 "$new_subnet_mask" || exit 1
        for value in $new_routers $new_domain_name_servers; do
            ipv4 "$value" || exit 1
        done
        if [ "$old_routers" != "$new_routers" ]; then remove_router; fi
        # Some DHCP servers omit the broadcast option. MiNT's ifconfig
        # otherwise retains a classful broadcast when changing the mask.
        ip=$(ip_number "$new_ip_address")
        mask=$(ip_number "$new_subnet_mask")
        broadcast=$(((ip & mask) | (~mask & 0xffffffff)))
        printf -v broadcast '%d.%d.%d.%d' "$(((broadcast >> 24) & 255))" \
            "$(((broadcast >> 16) & 255))" "$(((broadcast >> 8) & 255))" "$((broadcast & 255))"
        /bin/ifconfig eth0 addr "$new_ip_address" netmask "$new_subnet_mask" broadaddr "$broadcast" up || exit 1
        # FreeMiNT flushes interface routes when ifconfig applies an address,
        # including an unchanged address during RENEW or REBIND.
        for router in $new_routers; do
            /bin/route add default eth0 gw "$router" || exit 1
            break
        done
        : > /ram/etc/shndns.tmp
        for dns in $new_domain_name_servers; do
            echo "nameserver $dns" >> /ram/etc/shndns.tmp
        done
        /bin/cat /ram/etc/shndns.tmp > /ram/etc/resolv.conf || exit 1
        if ! echo "$new_ip_address" > /c/SHNNET.IP; then
            echo 'Shinogi DHCP: could not save the last LAN address for Settings.' >&2
        fi
        echo "Shinogi DHCP: eth0 $new_ip_address mask $new_subnet_mask router $new_routers DNS $new_domain_name_servers"
        ;;
    EXPIRE|FAIL|RELEASE|STOP)
        : > /c/SHNNET.IP
        remove_router
        /bin/ifconfig eth0 down
        ;;
    TIMEOUT)
        # A stale lease is not evidence that this LAN is still available.
        exit 1
        ;;
    ARPCHECK|ARPSEND|MEDIUM)
        ;;
esac
exit 0
