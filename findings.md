# Findings
Original CHAT sequence is checked for gaps by every receiver. PRIVATE_CHAT will use an independent per-session increasing sequence to preserve broadcast gap diagnostics. Session send_mutex already protects whole frames and socket lifetime.
