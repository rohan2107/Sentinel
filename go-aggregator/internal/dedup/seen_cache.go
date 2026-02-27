// Package dedup provides a thread-safe, bounded LRU cache for deduplicating
// report hashes before they reach storage. It is the fast path: cache hits
// avoid a database round-trip entirely.
package dedup

import (
	lru "github.com/hashicorp/golang-lru/v2"
)

// SeenCache tracks recently processed report hashes. It uses an LRU eviction
// policy so memory usage is bounded by the configured capacity.
//
// All methods are safe for concurrent use.
type SeenCache struct {
	cache *lru.Cache[string, struct{}]
}

// New creates a SeenCache with the given capacity.
// Returns an error if size is less than 1.
func New(size int) (*SeenCache, error) {
	c, err := lru.New[string, struct{}](size)
	if err != nil {
		return nil, err
	}
	return &SeenCache{cache: c}, nil
}

// Contains reports whether hash is present in the cache.
// Does not update recency order — use only for read-only checks.
func (c *SeenCache) Contains(hash string) bool {
	return c.cache.Contains(hash)
}

// Add records hash in the cache, evicting the oldest entry if at capacity.
func (c *SeenCache) Add(hash string) {
	c.cache.Add(hash, struct{}{})
}
