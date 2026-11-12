FROM python:3.12-slim
RUN pip install --no-cache-dir msgpack==1.1.2
WORKDIR /app
COPY tools/backup_screen_boot.py tools/screen_media.py tools/motion_push.py ./
USER 65534:65534
ENTRYPOINT ["python", "motion_push.py"]
