ARG BASE_IMAGE=pytorch/pytorch:2.7.0-cuda12.6-cudnn9-runtime
FROM ${BASE_IMAGE}

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

WORKDIR /workspace
COPY requirements.txt /workspace/requirements.txt
RUN pip install --upgrade pip \
    && pip install --no-cache-dir -r /workspace/requirements.txt

ENV HF_HOME=/models \
    HUGGINGFACE_HUB_CACHE=/models \
    TRANSFORMERS_CACHE=/models

ARG BUILD_HASH=dev
LABEL birefnet.build_hash=$BUILD_HASH
