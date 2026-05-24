package main

import (
	"sync"
	"time"
)

// slidingWindow — простой ring-bucket для счётчика в течение последней минуты.
// 60 секунд = 60 целочисленных бакетов; на каждый Tick секунды старый бакет
// очищается. Вычитание ведётся при Add(), что амортизирует cost (нет фоновой
// горутины на каждый user).
type slidingWindow struct {
	buckets [60]int64
	lastSec int64
	sum     int64
	mu      sync.Mutex
}

// Add инкрементирует счётчик на n; возвращает суммарное значение в окне (последние 60s).
func (w *slidingWindow) Add(n int64) int64 {
	w.mu.Lock()
	defer w.mu.Unlock()

	now := time.Now().Unix()
	w.advance(now)
	w.buckets[now%60] += n
	w.sum += n
	return w.sum
}

// Sum — текущее значение в окне без инкремента.
func (w *slidingWindow) Sum() int64 {
	w.mu.Lock()
	defer w.mu.Unlock()
	w.advance(time.Now().Unix())
	return w.sum
}

func (w *slidingWindow) advance(now int64) {
	if w.lastSec == 0 {
		w.lastSec = now
		return
	}
	if now == w.lastSec {
		return
	}
	delta := now - w.lastSec
	if delta >= 60 {
		for i := range w.buckets {
			w.buckets[i] = 0
		}
		w.sum = 0
	} else {
		// Очищаем бакеты, прошедшие за delta секунд (исключая текущую — её мы не трогаем
		// до Add). Бакеты [(lastSec+1)%60 .. now%60] inclusive.
		for s := w.lastSec + 1; s <= now; s++ {
			idx := s % 60
			w.sum -= w.buckets[idx]
			w.buckets[idx] = 0
		}
	}
	w.lastSec = now
}
