FROM pytorch/pytorch:2.3.1-cuda12.1-cudnn8-runtime
RUN pip install --no-cache-dir "transformers==4.46.3" "accelerate>=0.33" "fastapi>=0.110" "uvicorn[standard]>=0.29" numpy
WORKDIR /app
COPY server.py /app/server.py
EXPOSE 8081
CMD ["python", "server.py"]
