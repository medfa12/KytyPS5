# Synthetic AVC/HEVC keyframes

`generate.py` encodes one solid-red 64x48 YUV420P frame using FFmpeg 8.1.1
libx264/libx265, then retains only parameter sets and the IDR NAL. Normal tests
use the checked-in header and do not require either encoder. No service content
or encrypted samples are included.

Tests construct the separately audited 13.60 config records (AVC public size
56, HEVC size72, 64-bit NAL-length width, parameter-set blobs, boolean blob).
Both Annex-B and length-prefixed AUs must decode real red pixels with the
original timestamp, survive input-buffer overwrite, drain EOS and restart
after flush. Malformed lengths and protected AUs must be rejected atomically.

Additional HEVC fixtures exercise 10-bit BT.709 SDR conversion and explicit
PQ rejection. The SDR fixture converts the source's BT.601 matrix to BT.709
before encoding; the PQ-tagged fixture is rejection-only, not a reference HDR
image. Both Annex-B and length-prefixed modes are tested.
# Ordered configuration fixture

`generate-transition.py` generates `transition.h`: six synthetic red BT.709 AVC
frames at each of 64x48 and 96x64, with fixed two-B-frame reordering. Parameter
sets are separate from Annex-B AUs. Tests attach timestamps in decode order and
assert all presentation-order frames survive a changed configuration. Regenerate
with FFmpeg/libx264; normal tests use only the checked-in header.
