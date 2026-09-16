package dedup_test

import (
	"fmt"
	"sync"
	"testing"

	"github.com/rohan2107/sentinel/go-aggregator/internal/dedup"
)

func TestNew_InvalidSize(t *testing.T) {
	_, err := dedup.New(0)
	if err == nil {
		t.Fatal("expected error for size 0, got nil")
	}
}

func TestContains_UnseenHash(t *testing.T) {
	c, err := dedup.New(10)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	if c.Contains("deadbeef") {
		t.Fatal("Contains: expected false for unseen hash")
	}
}

func TestAddThenContains(t *testing.T) {
	c, err := dedup.New(10)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	c.Add("abc123")
	if !c.Contains("abc123") {
		t.Fatal("Contains: expected true after Add")
	}
}

func TestAdd_Idempotent(t *testing.T) {
	c, err := dedup.New(10)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	c.Add("abc123")
	c.Add("abc123") // second Add must not panic or duplicate
	if !c.Contains("abc123") {
		t.Fatal("Contains: expected true after double Add")
	}
}

func TestLRUEviction(t *testing.T) {
	c, err := dedup.New(3)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	c.Add("a")
	c.Add("b")
	c.Add("c")
	c.Add("d") // "a" is the oldest and must be evicted

	if c.Contains("a") {
		t.Fatal("expected 'a' to be evicted after capacity exceeded")
	}
	for _, h := range []string{"b", "c", "d"} {
		if !c.Contains(h) {
			t.Fatalf("expected %q to still be present", h)
		}
	}
}

func TestContains_UpdatesRecency(t *testing.T) {
	c, err := dedup.New(3)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	c.Add("a")
	c.Add("b")
	c.Add("c")
	if !c.Contains("a") {
		t.Fatal("expected 'a' to be present before recency touch")
	}
	c.Add("d")

	if c.Contains("b") {
		t.Fatal("expected 'b' to be evicted after touching 'a' and adding 'd'")
	}
	if !c.Contains("a") {
		t.Fatal("expected 'a' to remain due to recency touch")
	}
}

func TestConcurrentAddContains(t *testing.T) {
	c, err := dedup.New(1000)
	if err != nil {
		t.Fatalf("New: %v", err)
	}

	var wg sync.WaitGroup
	for i := 0; i < 200; i++ {
		wg.Add(1)
		go func(n int) {
			defer wg.Done()
			hash := fmt.Sprintf("hash%04d", n)
			c.Add(hash)
			c.Contains(hash)
		}(i)
	}
	wg.Wait()
}

func TestContainsOrAdd_Atomic(t *testing.T) {
	c, err := dedup.New(10)
	if err != nil {
		t.Fatalf("New: %v", err)
	}

	const goroutines = 100
	var wg sync.WaitGroup
	seenCount := 0
	var seenMu sync.Mutex

	for i := 0; i < goroutines; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if c.ContainsOrAdd("same-hash") {
				seenMu.Lock()
				seenCount++
				seenMu.Unlock()
			}
		}()
	}
	wg.Wait()

	if seenCount != goroutines-1 {
		t.Fatalf("expected %d duplicate hits, got %d", goroutines-1, seenCount)
	}
}
