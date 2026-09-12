FROM ubuntu:22.04

# Avoid prompts during apt installs
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libssl-dev \
    curl \
    wget \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy source code
COPY . .

# Build the project
RUN ./build.sh

# Expose HTTP Server and Web IDE ports
EXPOSE 8080
EXPOSE 8081

# Set the default command to run the Teal server
CMD ["./build/teal"]
