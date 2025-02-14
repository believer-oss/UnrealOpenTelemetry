docker pull otel/opentelemetry-collector-contrib:0.111.0
docker run otel/opentelemetry-collector-contrib:0.111.0
docker run -p 4317:4317 --net=host -v .\config.yaml:/etc/otelcol-contrib/config.yaml otel/opentelemetry-collector-contrib:0.111.0
