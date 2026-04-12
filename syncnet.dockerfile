FROM pytorch/pytorch:2.9.1-cuda12.6-cudnn9-runtime

ENV DEBIAN_FRONTEND=noninteractive
ENV PIP_NO_CACHE_DIR=1
ENV PYTHONUNBUFFERED=1

RUN apt-get update && apt-get install -y --no-install-recommends \
    ffmpeg \
    git \
    libsndfile1 \
    && rm -rf /var/lib/apt/lists/*

RUN python -m pip install --upgrade pip && \
    python -m pip install \
    numpy \
    scipy \
    opencv-python-headless \
    librosa \
    soundfile

WORKDIR /workspace

COPY sync_detector.py /workspace/sync_detector.py
RUN chmod +x /workspace/sync_detector.py

# Optional at runtime:
#   - mount your SyncNet checkpoint into /models
#   - pass args like:
#     --backend syncnet --syncnet-model /models/syncnet.pt --syncnet-device cuda
ENTRYPOINT ["python", "/workspace/sync_detector.py"]
