#pragma once
// La misma secuencia se usa en el firmware y en las pruebas con fallos inyectados.
// Antes de Ready no se inicializan salidas, radio, Wi-Fi ni Azure.
namespace gw {
enum class StartupFailure { None, State, Mutex, Queue, LocalTask };

inline const char* startupFailureName(StartupFailure f) {
  switch (f) {
    case StartupFailure::State: return "gateway_state";
    case StartupFailure::Mutex: return "mutex";
    case StartupFailure::Queue: return "request_queue";
    case StartupFailure::LocalTask: return "bioiot_local";
    default: return "none";
  }
}

template <typename Ops>
StartupFailure allocateStartupResources(Ops& ops) {
  if (!ops.createState()) return StartupFailure::State;
  ops.createPublishBuffer();  // fallo recuperable: operacion local sin publicar
  if (!ops.createMutex()) return StartupFailure::Mutex;
  if (!ops.createQueue()) return StartupFailure::Queue;
  // La tarea espera una notificacion: no observa una inicializacion parcial.
  if (!ops.createLocalTask()) return StartupFailure::LocalTask;
  return StartupFailure::None;
}
}  // namespace gw
