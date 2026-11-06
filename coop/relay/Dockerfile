# The co-op relay as a private server (README.md, "Running your own server").
#   docker build -t chronicle-coop-relay coop/relay
#   docker run -d --restart unless-stopped -p 7777:7777 chronicle-coop-relay
FROM node:22-alpine
WORKDIR /relay
COPY package.json package-lock.json ./
RUN npm ci --omit=dev --no-audit --no-fund
COPY server.mjs ./
COPY src/room.js ./src/
USER node
EXPOSE 7777
ENV PORT=7777 HOST=0.0.0.0
CMD ["node", "server.mjs"]
