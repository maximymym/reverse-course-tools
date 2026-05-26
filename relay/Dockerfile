FROM golang:1.25-alpine AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY *.go ./
RUN CGO_ENABLED=0 go build -ldflags="-s -w" -o /relay .

FROM alpine:3.19
RUN apk add --no-cache ca-certificates wget
COPY --from=build /relay /relay
EXPOSE 5050 5051
ENTRYPOINT ["/relay"]
