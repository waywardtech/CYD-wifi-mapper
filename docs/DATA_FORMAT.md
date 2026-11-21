# SD card data format

Everything lives under `/wifimap/` on a FAT32 card. One folder per scan set:

```
/wifimap/<slug>/project.json    floors + placed objects (rewritten on change)
/wifimap/<slug>/samples.jsonl   measurements, append-only, one JSON object per line
```

All lengths are metres. Plan coordinates are x right, y down (as on the plan image), relative
to the floor's anchor point. Use the same real-world anchor on every floor so they stack.

## project.json

```json
{ "v": 1, "name": "Whole House",
  "floors": [
    { "id": 1, "name": "Ground floor", "level": 0, "height": 2.5,
      "outline": [[0,0],[12,0],[12,6],[8,6],[8,9],[0,9]],
      "walls": [[5,0,5,3],[0,4,3.5,4]] }
  ],
  "objects": [
    { "name": "Router 1", "kind": 0, "floor": 1, "x": 1.0, "y": 8.3, "z": 0.5,
      "bssids": ["aa:bb:cc:dd:ee:01"] }
  ] }
```

- `floors[].id` is stable and is what `objects[].floor` and samples refer to. Never reuse an id.
- `level`: −1 basement, 0 ground, 1 first… Floors are shown and stacked in level order.
- `outline` is a closed polygon (the heat field is clipped to it); `walls` are `[x1,y1,x2,y2]`.
- `kind`: 0 Router, 1 Mesh node, 2 Switch, 3 Speaker, 4 TV, 5 Console, 6 Other.
- `z` is height above that floor. `bssids` are the radios the object broadcasts on.

The web app owns `floors`; the CYD owns `objects`. Each re-reads the file before saving its part.

## samples.jsonl

```json
{"t":1794700123,"f":1,"p":[3.25,4.5,1.2],"c":["aa:bb:cc:dd:ee:02",-61],"a":[["aa:bb:cc:dd:ee:01",-48,6,"HomeNet"],["5c:00:00:00:00:03",-77,11,"Neighbour"]]}
{"undo":1}
```

- `t` unix time (0 if the clock was never set), `f` floor id, `p` `[x,y,z]` of the device.
- `c` (optional) `[bssid, rssi]` of the access point the CYD joined for this sample.
- `a` up to 24 access points heard, strongest first: `[bssid, rssi dBm, channel, ssid]`.
  RSSI is the mean of three scans.
- `{"undo":1}` removes the sample before it. The file is never rewritten, only appended to.

Every access point heard is stored, not just the bound ones, so binding a device later
applies to measurements already taken.
