Import("env")

def after_upload(source, target, env):
    print("\n[INFO] Subiendo SPIFFS automáticamente después del firmware...")
    env.Execute("pio run --target uploadfs")

# Ejecutar después de 'upload'
env.AddPostAction("upload", after_upload)
