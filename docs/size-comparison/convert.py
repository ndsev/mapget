# pip install pymongo msgpack

import json
import bson
import msgpack

# Load JSON data
with open('tile.json', 'r') as f:
    data = json.load(f)

# Save as BSON
with open('output.bson', 'wb') as f:
    f.write(bson.BSON.encode(data))

# Save as MessagePack
with open('output.msgpack', 'wb') as f:
    f.write(msgpack.packb(data, use_bin_type=True))
