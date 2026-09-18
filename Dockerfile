FROM ubuntu:24.04 AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends cmake g++ make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)" \
    && ctest --test-dir build --output-on-failure

FROM ubuntu:24.04

RUN useradd --create-home --uid 10001 cppkv \
    && mkdir -p /data \
    && chown cppkv:cppkv /data

COPY --from=build /src/build/cppkv_server /usr/local/bin/cppkv_server

USER cppkv
EXPOSE 6380
VOLUME ["/data"]
ENTRYPOINT ["/usr/local/bin/cppkv_server"]
CMD ["--port", "6380", "--threads", "8", "--aof", "/data/cppkv.aof"]
