FROM debian:bookworm-slim

# Install dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    git \
    gcc \
    libc6-dev \
    python3 \
    gosu \
    procps \
    strace \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Install gh CLI
RUN curl -fsSL https://cli.github.com/packages/githubcli-archive-keyring.gpg -o /usr/share/keyrings/githubcli-archive-keyring.gpg \
    && echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" > /etc/apt/sources.list.d/github-cli.list \
    && apt-get update && apt-get install -y gh \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user
RUN useradd -ms /bin/bash agent

# Compile the credential server
COPY credential-server.c /tmp/credential-server.c
RUN gcc -O2 -Wall -o /usr/local/bin/credential-server /tmp/credential-server.c \
    && rm /tmp/credential-server.c \
    && chmod 755 /usr/local/bin/credential-server

# Install the credential helper client (Python script)
COPY git-credential-daimon /usr/local/bin/git-credential-daimon
RUN chmod 755 /usr/local/bin/git-credential-daimon

# Configure git to use our credential helper (system-wide)
RUN git config --system credential.helper '/usr/local/bin/git-credential-daimon'

# Pre-create gh CLI config for agent user (so we don't need DAC_OVERRIDE at runtime)
RUN mkdir -p /home/agent/.config/gh && \
    printf 'github.com:\n    git_protocol: https\n    users:\n        x-access-token:\n    user: x-access-token\n' > /home/agent/.config/gh/hosts.yml && \
    chown -R agent:agent /home/agent/.config

# Install entrypoint
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod 755 /usr/local/bin/entrypoint.sh

# The secret will be mounted at runtime
# /run/secrets/gh_token - mode 600, owned by root

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
