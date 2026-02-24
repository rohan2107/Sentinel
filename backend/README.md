# Sentinel Backend

FastAPI-based backend for receiving and storing security compliance reports.

## Features

- **Hash-based deduplication**: Reports identified by SHA-256 hash
- **Idempotent delivery**: Returns 409 for duplicates (treated as success)
- **Hash verification**: Validates submitted hash matches report content
- **SQLite persistence**: Stores reports in `backend.db`
- **RESTful API**: Standard HTTP endpoints

## Installation

```bash
cd backend
pip install -r requirements.txt
```

## Running the Server

```bash
# Development mode (auto-reload)
uvicorn server:app --reload --port 8000

# Production mode
python server.py
```

## API Endpoints

### POST /reports
Submit a compliance report.

**Request Body:**
```json
{
  "report": {
    "timestamp": "2026-02-24T12:00:00.000Z",
    "hostname": "production-server",
    "policy": "windows-security-baseline",
    "score": 85,
    "details": {"firewall_enabled": true, "av_installed": true}
  },
  "hash": "abc123..."
}
```

**Response:**
- `200 OK`: Report accepted
- `409 Conflict`: Duplicate (already received)
- `400 Bad Request`: Hash mismatch

### GET /reports/{hash}
Retrieve a report by its SHA-256 hash.

### GET /reports
List all reports (paginated).

**Query Parameters:**
- `limit`: Number of reports (default: 50)
- `offset`: Pagination offset (default: 0)

### GET /health
Health check endpoint.

## Testing

```bash
# Start backend
uvicorn server:app --port 8000

# Submit a report (from agent)
.\build\Release\Sentinel.exe --enable-delivery --backend-url http://localhost:8000

# List reports
curl http://localhost:8000/reports

# Get specific report
curl http://localhost:8000/reports/{hash}
```

## Database Schema

```sql
CREATE TABLE received_reports (
    report_hash TEXT PRIMARY KEY,
    report_json TEXT NOT NULL,
    received_at TEXT NOT NULL,
    hostname TEXT,
    policy TEXT,
    score INTEGER
);
```

## Idempotency

The backend ensures idempotent delivery:
1. Agent computes SHA-256 hash of report
2. Backend checks if hash exists
3. If exists, returns 409 (duplicate)
4. Agent treats 409 as success (already delivered)

This enables crash-safe at-least-once delivery semantics without duplicate storage.
