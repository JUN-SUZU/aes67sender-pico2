# PTP / AES67 test

この版は既存のUSB Audio ring buffer、PWMローカル再生、CDC CLIを残し、PTP同期時だけring bufferのconsumerをAES67へ切り替えます。

## 主要コマンド

```text
config show
config set ip 192.168.2.5
config set multicast auto
config set port 5004
config set domain 0
config apply

ptp
ptp reset-stats
aes67
sap
tui
```

`config save` は設定を適用してFlashへ保存します。USB Audio streaming中はFlash操作を拒否します。

## 送信条件

1. Ethernet linkがUP
2. PTPがLOCKED
3. USB Audioが48 kHzでSTREAMING
4. ring bufferが192 framesまで到達

上記を満たすと、PTP時刻の1 ms境界で48 stereo framesずつ送信します。

- RTP payload type: 96
- Format: L24, 48 kHz, stereo
- Packet time: 1 ms
- UDP payload: 300 bytes（RTP header 12 + audio 288）
- Default destination: `239.69.2.5:5004`
- RTP DSCP: EF (46)

元データは16-bit PCMなので、L24の上位16 bitへ配置し、下位8 bitは0です。

Wiresharkでは次で確認できます。

```text
udp.port == 5004
```

動的payload typeのため、必要なら対象UDPストリームをRTPとしてDecode Asしてください。正常時は約1000 packets/s、RTP timestampはpacketごとに48増えます。

## SDP

AES67が`STREAMING`へ入ると、`239.255.255.255:9875`へSAP広告を即時送信し、その後30秒ごとに再送します。SAP packet内のSDPには、適用中のIP、multicast、RTP port、stream名、PTP GM Clock ID、domainが自動的に入ります。

Wiresharkでの確認:

```text
udp.port == 9875 || udp.port == 5004
```

Ethernet headerの宛先MACも確認してください。

```text
SAP  239.255.255.255 -> 01:00:5e:7f:ff:ff
RTP  239.69.2.5     -> 01:00:5e:45:02:05
```

IP宛先がmulticastでもEthernet宛先がgatewayのunicast MACなら、switchはmulticastとして配信できません。この版ではSAP/RTP socketをW5500のmulticast modeでOPENします。

`aes67-default.sdp` はSAPを受け取れないreceiverへ手動で渡すための予備ファイルです。SAP対応のAES67-monitorを使う場合は通常不要です。

## 現段階の範囲

RTP/L24送信、PTP連動、pre-buffer、SAP/SDP広告、監視までを実装しています。完全なBMCAと音声resamplingはまだ含みません。長時間試験ではring fill、underrun/overflow、phase min/max/averageを監視してください。
