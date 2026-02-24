# Quick Start: Delivery Layer

This guide shows how to use the new HTTP delivery functionality.

## Prerequisites

1. **Install cpp-httplib** (header-only HTTP library):
   ```powershell
   vcpkg install cpp-httplib
   ```

2. **Rebuild the project**:
   ```powershell
   .\scripts\build.ps1
   ```

3. **Install Python backend dependencies**:
   ```powershell
   cd backend
   pip install -r requirements.txt
   ```

## Running End-to-End

### Terminal 1: Start the Backend

```powershell
cd backend
uvicorn server:app --reload --port 8000
```

The backend will be available at `http://localhost:8000`.

**API Endpoints:**
- `POST /reports` - Submit a report
- `GET /reports` - List all reports
- `GET /reports/{hash}` - Get specific report by hash
- `GET /health` - Health check

### Terminal 2: Run Sentinel with Delivery

```powershell
# Basic run (no delivery, existing behavior)
.\build\Release\Sentinel.exe

# With delivery enabled
.\build\Release\Sentinel.exe --enable-delivery --backend-url http://localhost:8000
```

**Command-line Options:**
- `--enable-delivery` - Activate delivery layer
- `--backend-url <url>` - Backend endpoint (default: `http://localhost:8000`)
- `--policy <path>` - Policy file (default: `policies/sample_policy.json`)

## What Happens

1. **Startup Crash Recovery**: Agent checks for pending reports from previous runs and retries delivery
2. **Policy Evaluation**: Runs osquery queries and Lua evaluation (existing behavior)
3. **Report Generation**: Creates JSON report with score and details
4. **Hash Computation**: Computes SHA-256 hash of report (content-addressable)
5. **Enqueue**: Persists report to `retry_queue` table
6. **Immediate Delivery**: Attempts HTTP POST to backend
7. **Success**: Marks as `DELIVERED` in database
8. **Failure**: Schedules retry with exponential backoff (1s → 2s → 4s → ... → 300s max)

## Viewing Results

### In the Backend

```bash
# List all reports
curl http://localhost:8000/reports

# Get specific report
curl http://localhost:8000/reports/{hash}
```

### In the Agent Database

```sql
-- Check retry queue status
SELECT run_id, state, attempts, delivered_at, last_error 
FROM retry_queue 
ORDER BY created_at DESC;

-- View delivered reports
SELECT * FROM retry_queue WHERE state = 'DELIVERED';

-- View failed reports
SELECT * FROM retry_queue WHERE state = 'FAILED';

-- View pending retries
SELECT * FROM retry_queue WHERE state = 'PENDING';
```

## Testing Scenarios

### 1. Success Path
```powershell
# Backend running
.\build\Release\Sentinel.exe --enable-delivery
# Check backend: report should appear immediately
```

### 2. Backend Down (Retry Logic)
```powershell
# Stop backend (Ctrl+C in Terminal 1)
.\build\Release\Sentinel.exe --enable-delivery
# Report goes to PENDING state with retry schedule

# Start backend again
uvicorn server:app --port 8000

# Run agent again (crash recovery kicks in)
.\build\Release\Sentinel.exe --enable-delivery
# Pending report should be delivered
```

### 3. Idempotency (Duplicate Detection)
```powershell
# Same policy evaluation produces same hash
.\build\Release\Sentinel.exe --enable-delivery
.\build\Release\Sentinel.exe --enable-delivery  # Same report
# Backend returns 409 (duplicate), agent treats as success
# Only one report stored in backend
```

### 4. Network Timeout
```powershell
# Backend slow/unresponsive
# Agent will timeout after 30s, schedule retry
```

## Troubleshooting

### Agent logs "Delivery failed"
- Check if backend is running: `curl http://localhost:8000/health`
- Check backend logs for errors
- Verify network connectivity
- Check `retry_queue` table for error messages

### Backend rejects with "Hash mismatch"
- Report was modified/tampered
- Hash computation differs between agent and backend
- This should never happen in normal operation

### Reports stuck in PENDING
- Check `next_retry_at` timestamp - may be scheduled for future
- Check `attempts` - after 10 attempts, moves to FAILED
- Run agent again to trigger `process_pending()`

### Database locked errors
- SQLite WAL mode is enabled, but concurrent access may still cause issues
- Ensure only one agent instance runs at a time

## Architecture Summary

```
Agent (Sentinel.exe)
  └─> Generate Report
       └─> Compute SHA-256 Hash
            └─> Persist to retry_queue (PENDING)
                 └─> HTTP POST to Backend
                      ├─> Success (200/201) → DELIVERED
                      ├─> Duplicate (409) → DELIVERED (idempotent)
                      ├─> Server Error (5xx) → Retry with backoff
                      └─> Client Error (4xx) → FAILED (terminal)

Backend (FastAPI)
  └─> Receive Report
       └─> Verify Hash
            └─> Check Duplicate
                 ├─> Exists → 409 Conflict
                 └─> New → Store + 200 OK
```

## Next Steps

- **Production Deployment**: Update `--backend-url` to production endpoint
- **Monitoring**: Add logging/metrics around delivery success rate
- **MQTT Support**: Add `MqttDeliveryClient` for message broker delivery
- **Authentication**: Add API key/token to backend requests
- **TLS**: Use `https://` URLs for encrypted transport

## Files Changed

- `src/http_delivery_client.{h,cpp}` - HTTP client implementation
- `src/retry_queue.{h,cpp}` - Retry manager with exponential backoff
- `src/main.cpp` - Integration with delivery layer
- `backend/server.py` - FastAPI backend
- `CMakeLists.txt` - Added cpp-httplib dependency
- `test_delivery_foundation.cpp` - Integration tests

See [`docs/roadmap/IMPLEMENTATION_PLAN.md`](docs/roadmap/IMPLEMENTATION_PLAN.md) for detailed implementation notes.
