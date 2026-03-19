ARG BUILDER_IMAGE=ubuntu:24.04
ARG RUNTIME_IMAGE=ubuntu:24.04

FROM ${BUILDER_IMAGE} AS builder
LABEL maintainer="w180112@gmail.com"
USER root

ADD . /fastrg

RUN /fastrg/essentials.sh \
    && /fastrg/boot.sh

# ---- Runtime Stage ----
ARG RUNTIME_IMAGE=ubuntu:24.04
FROM ${RUNTIME_IMAGE} AS runtime
USER root

WORKDIR /fastrg

COPY --from=builder /etc/fastrg/ /etc/fastrg/
COPY --from=builder --chown=root:root --chmod=0755 /usr/local/bin/fastrg /usr/local/bin/fastrg
COPY --from=builder /usr/local/lib/libutils.so.*.*.* /usr/local/lib/
COPY --from=builder /usr/local/lib/libetcd-cpp-api.so /usr/local/lib/
COPY --from=builder /usr/local/include/etcd /usr/local/include/
COPY --from=builder --chown=root:root --chmod=0755 /usr/local/bin/fastrg_cli /usr/local/bin/

RUN mkdir -p /var/log/fastrg && mkdir -p /var/run/fastrg \
    && ln -s /usr/local/lib/libutils.so.* /usr/local/lib/libutils.so \
    && apt update -y && apt install -y \
       libnuma1 libatomic1 libconfig9 iproute2 \
       libgrpc++1.51t64 libjsoncpp25 libcpprest2.10 \
       libarchive-dev \
    && apt clean -y && apt autoclean -y && apt autoremove -y \
    && rm -rf /var/lib/apt/lists/*

VOLUME /var/log/fastrg
VOLUME /var/run/fastrg
VOLUME /etc/fastrg
