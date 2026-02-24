# backend/server.py
from fastapi import FastAPI, HTTPException
from fastapi.responses import JSONResponse
from pydantic import BaseModel
import sqlite3
import hashlib
import json
from datetime import datetime, timezone
from typing import Any

app = FastAPI(title="Sentinel Backend", version="1.0.0")

# Initialize database
db_path = "backend.db"

def init_db():
    """Create database schema if not exists"""
    conn = sqlite3.connect(db_path)
    cursor = conn.cursor()
    
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS received_reports (
            report_hash TEXT PRIMARY KEY,
            report_json TEXT NOT NULL,
            received_at TEXT NOT NULL,
            hostname TEXT,
            policy TEXT,
            score INTEGER
        )
    """)
    
    conn.commit()
    conn.close()

# Initialize on startup
init_db()

class ReportSubmission(BaseModel):
    report: dict[str, Any]
    hash: str

@app.post("/reports")
async def receive_report(submission: ReportSubmission) -> JSONResponse | dict[str, Any]:
    """
    Receive and store a compliance report.
    Returns 200 for new reports, 409 for duplicates.
    Verifies hash matches report content.
    """
    # Verify hash matches content
    canonical = json.dumps(submission.report, sort_keys=True, separators=(',', ':'))
    computed_hash = hashlib.sha256(canonical.encode()).hexdigest()
    
    if computed_hash != submission.hash:
        raise HTTPException(
            status_code=400,
            detail=f"Hash mismatch: expected {submission.hash}, computed {computed_hash}"
        )
    
    # Check for duplicate
    conn = None
    try:
        conn = sqlite3.connect(db_path)
        cursor = conn.cursor()
        
        cursor.execute("SELECT report_hash FROM received_reports WHERE report_hash = ?", (submission.hash,))
        existing = cursor.fetchone()
        
        if existing:
            return JSONResponse(
                status_code=409,
                content={
                    "status": "duplicate",
                    "message": "Report already received (idempotent)",
                    "hash": submission.hash
                }
            )
        
        # Store report
        try:
            cursor.execute("""
                INSERT INTO received_reports (report_hash, report_json, received_at, hostname, policy, score)
                VALUES (?, ?, ?, ?, ?, ?)
            """, (
                submission.hash,
                json.dumps(submission.report),
                datetime.now(timezone.utc).isoformat(),
                submission.report.get('hostname', 'unknown'),
                submission.report.get('policy', 'unknown'),
                submission.report.get('score', 0)
            ))
            
            conn.commit()
            
            return {
                "status": "accepted",
                "message": "Report stored successfully",
                "hash": submission.hash,
                "score": submission.report.get('score', 0)
            }
            
        except sqlite3.IntegrityError as e:
            # Race condition: another request inserted the same hash
            return JSONResponse(
                status_code=409,
                content={
                    "status": "duplicate",
                    "message": "Report already received (race condition)",
                    "hash": submission.hash
                }
            )
        except Exception as e:
            raise HTTPException(status_code=500, detail=f"Database error: {str(e)}")
    finally:
        if conn:
            conn.close()

@app.get("/reports/{report_hash}")
async def get_report(report_hash: str) -> dict[str, Any]:
    """Retrieve a report by its hash"""
    conn = sqlite3.connect(db_path)
    cursor = conn.cursor()
    
    cursor.execute("""
        SELECT report_json, received_at, hostname, policy, score
        FROM received_reports
        WHERE report_hash = ?
    """, (report_hash,))
    
    row = cursor.fetchone()
    conn.close()
    
    if not row:
        raise HTTPException(status_code=404, detail="Report not found")
    
    report_json, received_at, hostname, policy, score = row
    
    return {
        "hash": report_hash,
        "report": json.loads(report_json),
        "received_at": received_at,
        "hostname": hostname,
        "policy": policy,
        "score": score
    }

@app.get("/reports")
async def list_reports(limit: int = 50, offset: int = 0) -> dict[str, Any]:
    """List all received reports (paginated)"""
    # Validate input parameters
    if limit < 1 or limit > 1000:
        raise HTTPException(status_code=400, detail="Limit must be between 1 and 1000")
    if offset < 0:
        raise HTTPException(status_code=400, detail="Offset must be non-negative")
    
    conn = sqlite3.connect(db_path)
    cursor = conn.cursor()
    
    cursor.execute("""
        SELECT report_hash, received_at, hostname, policy, score
        FROM received_reports
        ORDER BY received_at DESC
        LIMIT ? OFFSET ?
    """, (limit, offset))
    
    rows = cursor.fetchall()
    
    cursor.execute("SELECT COUNT(*) FROM received_reports")
    total: int = cursor.fetchone()[0]
    
    conn.close()
    
    reports: list[dict[str, Any]] = []
    for row in rows:
        reports.append({
            "hash": row[0],
            "received_at": row[1],
            "hostname": row[2],
            "policy": row[3],
            "score": row[4]
        })
    
    return {
        "total": total,
        "limit": limit,
        "offset": offset,
        "reports": reports
    }

@app.get("/health")
async def health_check() -> dict[str, str]:
    """Health check endpoint"""
    return {"status": "healthy", "service": "sentinel-backend"}

@app.get("/")
async def root() -> dict[str, Any]:
    """Root endpoint with API information"""
    return {
        "service": "Sentinel Backend",
        "version": "1.0.0",
        "endpoints": {
            "POST /reports": "Submit a compliance report",
            "GET /reports/{hash}": "Retrieve a report by hash",
            "GET /reports": "List all reports (paginated)",
            "GET /health": "Health check"
        }
    }

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
