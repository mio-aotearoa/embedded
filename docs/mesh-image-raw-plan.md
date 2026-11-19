# Mesh image messages (not on-device decode)

Chunk transport (MeshCore `GRP_DATA`) is small; the upstream codec needs an ONNX
bundle (~1 GB) and peaks above 2 GB RAM. The S3 cannot reconstruct pixels.

Practical options: reassemble and save `.mimg` to SD for a phone/desktop; or
have a companion decode and push a downscaled JPEG/PNG back. No T-Deck image
bubble is wired yet.
