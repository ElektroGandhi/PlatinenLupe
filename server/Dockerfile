# Platinenlupe server — build from the repository root:  docker build -f server/Dockerfile .
FROM python:3.12-slim-bookworm
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 PL_WEB_DIR=/app/web
WORKDIR /app
COPY server/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY server/platinenlupe/ platinenlupe/
COPY web/index.html web/index.html
USER 65534:65534
EXPOSE 8080
HEALTHCHECK --interval=60s --timeout=5s --start-period=20s \
  CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:' + __import__('os').environ.get('PL_PORT', '8080') + '/healthz', timeout=4)"
CMD ["python", "-m", "platinenlupe"]
