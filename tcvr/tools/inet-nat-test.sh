#!/bin/bash
# tcvr: internet play test on one Linux computer (root): a tcvr-server on a fake internet, players behind
# fake home routers (NAT), with network namespaces. Needs ip (iproute2), iptables and the two programs
# built from tcvr/server (cmake -S tcvr/server -B build-server && cmake --build build-server).
#
#   sudo tcvr/tools/inet-nat-test.sh BUILD_DIR [scenario...]
#
# Scenarios (all when none given):
#   cone2       2 players, each behind a usual home router: they must reach each other directly
#   strict2     2 players, one behind a "symmetric" router (new port per destination): relay through the server
#   mixed4      4 players: two usual routers, one symmetric, and two players behind the same router;
#               80 ms round trip and 2% loss between players (TCVR_INET_NETEM, in the client)
#   handover3   a private game (PIN) of 3 players: the host leaves the lobby when all are in, the next one
#               hosts the same private game and starts the match
#   pin2        a private game: a joiner with the wrong PIN is refused ("Wrong PIN"), one with the right PIN plays
#
# The addresses are documentation ranges (198.51.100.0/24 "internet", 192.168.N.0/24 homes); nothing leaves
# this computer. Everything made here is removed at the end.
set -u
BUILD=${1:?build dir with tcvr-server and tcvr-inet-peer}
shift
SCEN=${*:-cone2 strict2 mixed4 handover3 pin2}
SERVER=$BUILD/tcvr-server
PEER=$BUILD/tcvr-inet-peer
LOG=${TCVR_TEST_LOG:-/tmp/tcvr-inet-test}
mkdir -p "$LOG"
NS_LIST=()

ns() { local n=$1; shift; ip netns exec "$n" "$@"; }

cleanup() {
	# ours, and any left by an earlier run that was stopped
	local all=$(ip netns list 2>/dev/null | awk '{print $1}' | grep -E '^(tsrv|tr[0-9]+|tp[a-z])$')
	for n in $all; do ip netns pids "$n" 2>/dev/null | xargs -r kill 2>/dev/null; done
	sleep 0.2
	for n in $all; do ip netns del "$n" 2>/dev/null; done
	for l in $(ip -o link show 2>/dev/null | awk -F': ' '{print $2}' | cut -d@ -f1 | grep -E '^(tcvrbr0|tsrv0b|tw[0-9]+b|te[a-z]b)$'); do
		ip link del "$l" 2>/dev/null
	done
	NS_LIST=()
	sleep 0.5
}
trap cleanup EXIT
trap "exit 1" INT TERM

# the internet: a bridge in this namespace; the server at .10
make_internet() {
	ip link add tcvrbr0 type bridge
	ip link set tcvrbr0 up
	ip netns add tsrv; NS_LIST+=(tsrv)
	ip link add tsrv0 type veth peer name tsrv0b
	ip link set tsrv0 netns tsrv
	ip link set tsrv0b master tcvrbr0 up
	ns tsrv ip addr add 198.51.100.10/24 dev tsrv0
	ns tsrv ip link set tsrv0 up
	ns tsrv ip link set lo up
}

# a home: router tr$1 (WAN 198.51.100.(20+$1)), NAT kind $2 (cone|symmetric)
make_home() {
	local h=$1 kind=$2
	ip netns add tr$h; NS_LIST+=(tr$h)
	ip link add tw$h type veth peer name tw${h}b
	ip link set tw$h netns tr$h
	ip link set tw${h}b master tcvrbr0 up
	ns tr$h ip addr add 198.51.100.$((20 + h))/24 dev tw$h
	ns tr$h ip link set tw$h up
	ns tr$h ip link set lo up
	ns tr$h ip link add tl$h type bridge
	ns tr$h ip addr add 192.168.$h.1/24 dev tl$h
	ns tr$h ip link set tl$h up
	ns tr$h sysctl -qw net.ipv4.ip_forward=1
	if [ "$kind" = symmetric ]; then
		ns tr$h iptables -t nat -A POSTROUTING -o tw$h -j MASQUERADE --random-fully
	else
		ns tr$h iptables -t nat -A POSTROUTING -o tw$h -j MASQUERADE
	fi
	# a home router lets in only what answers something sent out
	ns tr$h iptables -A FORWARD -i tw$h -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
	ns tr$h iptables -A FORWARD -i tw$h -j DROP
	# and drops what nobody asked for at its own address (else Linux keeps a trace of it that makes the
	# next outgoing packet to that sender take another port)
	ns tr$h iptables -A INPUT -i tw$h -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
	ns tr$h iptables -A INPUT -i tw$h -j DROP
}

# a player $2 in home $1 at 192.168.$1.(10+$3)
make_player() {
	local h=$1 p=$2 idx=$3
	ip netns add tp$p; NS_LIST+=(tp$p)
	ip link add te$p type veth peer name te${p}b
	ip link set te$p netns tp$p
	ip link set te${p}b netns tr$h
	ns tr$h ip link set te${p}b master tl$h up
	ns tp$p ip addr add 192.168.$h.$((10 + idx))/24 dev te$p
	ns tp$p ip link set te$p up
	ns tp$p ip link set lo up
	ns tp$p ip route add default via 192.168.$h.1
}

start_server() {
	ns tsrv "$SERVER" --port 19800 --key "$LOG/server.key" --verbose > "$LOG/$1-server.log" 2>&1 &
	sleep 0.5
}

# run_players scenario "ns:name:args..." ... ; returns 0 when every player that must succeed did
run_players() {
	local sc=$1; shift
	local pids=() names=()
	for spec in "$@"; do
		local n=${spec%%:*}; local rest=${spec#*:}
		local name=${rest%%:*}; local args=${rest#*:}
		ns "$n" env TCVR_INET_NETEM="${NETEM:-}" "$PEER" --server 198.51.100.10:19800 --name "$name" $args > "$LOG/$sc-$name.log" 2>&1 &
		pids+=($!); names+=("$name")
		sleep 0.4
	done
	local rc=0 i=0
	for pid in "${pids[@]}"; do
		wait "$pid" || rc=1
		echo "  $(grep RESULT "$LOG/$sc-${names[$i]}.log" || echo "RESULT name=${names[$i]} ok=0 why=no-result")"
		i=$((i + 1))
	done
	return $rc
}

PASS=0; FAIL=0
result() { if [ "$2" = 0 ]; then echo "PASS $1"; PASS=$((PASS + 1)); else echo "FAIL $1"; FAIL=$((FAIL + 1)); fi; }

for sc in $SCEN; do
	cleanup
	make_internet
	case $sc in
	cone2)
		make_home 1 cone; make_home 2 cone
		make_player 1 a 1; make_player 2 b 1
		start_server $sc
		run_players $sc "tpa:AAA:--host --players 2 --seconds 15" "tpb:BBB:--join --players 2 --seconds 15"
		rc=$?
		# both directions must have gone direct
		grep -q "path=relay\|path=?" "$LOG/$sc-AAA.log" "$LOG/$sc-BBB.log" && rc=1
		result $sc $rc ;;
	strict2)
		make_home 1 cone; make_home 2 symmetric
		make_player 1 a 1; make_player 2 b 1
		start_server $sc
		run_players $sc "tpa:AAA:--host --players 2 --seconds 15" "tpb:BBB:--join --players 2 --seconds 15"
		result $sc $? ;;
	mixed4)
		make_home 1 cone; make_home 2 cone; make_home 3 symmetric
		make_player 1 a 1; make_player 2 b 1; make_player 3 c 1; make_player 1 d 2
		start_server $sc
		# every player delays its packets 40 ms and drops 1% (the kernel's netem may be missing): 80 ms round
		# trip and 2% loss on a direct path
		NETEM=40,1 run_players $sc "tpa:AAA:--host --players 4 --seconds 30" "tpb:BBB:--join --players 4 --seconds 30" \
			"tpc:CCC:--join --players 4 --seconds 30" "tpd:DDD:--join --players 4 --seconds 30"
		result $sc $? ;;
	handover3)
		make_home 1 cone; make_home 2 cone; make_home 3 cone
		make_player 1 a 1; make_player 2 b 1; make_player 3 c 1
		start_server $sc
		run_players $sc "tpa:AAA:--host --players 3 --leave-at-full --seconds 15 --pin 4321" \
			"tpb:BBB:--join --players 2 --seconds 15 --pin 4321" "tpc:CCC:--join --players 2 --seconds 15 --pin 4321"
		rc=$?
		grep -q "role=host ok=1" "$LOG/$sc-BBB.log" || rc=1
		result $sc $rc ;;
	pin2)
		make_home 1 cone; make_home 2 cone; make_home 3 cone
		make_player 1 a 1; make_player 2 b 1; make_player 3 c 1
		start_server $sc
		# BBB tries 1111 and is refused; CCC comes with the right PIN and plays
		run_players $sc "tpa:AAA:--host --players 2 --seconds 8 --timeout 40 --pin 2580" \
			"tpb:BBB:--join --players 2 --seconds 8 --timeout 20 --pin 1111" \
			"tpc:CCC:--join --players 2 --seconds 8 --timeout 40 --pin 2580"
		rc=0
		grep -q "refused:Wrong PIN" "$LOG/$sc-BBB.log" || rc=1
		grep -q "ok=1" "$LOG/$sc-BBB.log" && rc=1
		grep -q "role=joiner ok=1" "$LOG/$sc-CCC.log" || rc=1
		grep -q "role=host ok=1" "$LOG/$sc-AAA.log" || rc=1
		grep -q "private" "$LOG/$sc-server.log" || rc=1
		result $sc $rc ;;
	*) echo "unknown scenario $sc"; FAIL=$((FAIL + 1)) ;;
	esac
done
echo "passed $PASS, failed $FAIL (logs in $LOG)"
[ $FAIL = 0 ]
