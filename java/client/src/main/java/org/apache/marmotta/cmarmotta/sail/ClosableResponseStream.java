package org.apache.marmotta.cmarmotta.sail;

import com.google.common.base.Preconditions;
import info.aduna.iteration.CloseableIteration;
import io.grpc.ClientCall;
import io.grpc.Metadata;
import io.grpc.MethodDescriptor;
import io.grpc.Status;
import io.grpc.stub.StreamObserver;
import org.apache.marmotta.cmarmotta.client.proto.SailServiceGrpc;
import org.openrdf.sail.SailException;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.util.NoSuchElementException;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;

/**
 * A modified version of ClientCalls.BlockingResponseStream that allows closing the stream early.
 *
 * @author Sebastian Schaffert (sschaffert@apache.org)
 */
public class ClosableResponseStream<ReqT, T> implements CloseableIteration<T, SailException> {

    private static Logger log = LoggerFactory.getLogger(ClosableResponseStream.class);

    // Due to flow control, only needs to hold up to 2 items: 1 for value, 1 for close.
    private final BlockingQueue<Object> buffer = new ArrayBlockingQueue<Object>(2);
    private final ClientCall.Listener<T> listener = new QueuingListener();
    private final ClientCall<ReqT, T> call;
    // Only accessed when iterating.
    private Object last;

    ClosableResponseStream(SailServiceGrpc.SailServiceStub stub, MethodDescriptor<ReqT, T> method, ReqT req) throws SailException {
        call = stub.getChannel().newCall(method, stub.getCallOptions());

        call.start(listener(), new Metadata());
        call.request(1);
        try {
            call.sendMessage(req);
            call.halfClose();
        } catch (Throwable t) {
            call.cancel();
            throw new SailException(t);
        }
    }

    ClientCall.Listener<T> listener() {
        return listener;
    }

    /**
     * Closes this iteration, freeing any resources that it is holding. If the
     * iteration has already been closed then invoking this method has no effect.
     */
    @Override
    public void close() throws SailException {
        call.cancel();
    }

    /**
     * Returns <tt>true</tt> if the iteration has more elements. (In other
     * words, returns <tt>true</tt> if {@link #next} would return an element
     * rather than throwing a <tt>NoSuchElementException</tt>.)
     *
     * @return <tt>true</tt> if the iteration has more elements.
     * @throws SailException
     */
    @Override
    public boolean hasNext() throws SailException {
        try {
            // Will block here indefinitely waiting for content. RPC timeouts defend against permanent
            // hangs here as the call will become closed.
            last = (last == null) ? buffer.take() : last;
        } catch (InterruptedException ie) {
            Thread.interrupted();
            throw new SailException(ie);
        }
        if (last instanceof Status) {
            throw new SailException(((Status) last).asRuntimeException());
        }
        return last != this;
    }

    /**
     * Returns the next element in the iteration.
     *
     * @return the next element in the iteration.
     * @throws NoSuchElementException if the iteration has no more elements or if it has been closed.
     */
    @Override
    public T next() throws SailException {
        if (!hasNext()) {
            throw new NoSuchElementException();
        }
        try {
            call.request(1);
            @SuppressWarnings("unchecked")
            T tmp = (T) last;
            return tmp;
        } finally {
            last = null;
        }
    }

    /**
     * Removes from the underlying collection the last element returned by the
     * iteration (optional operation). This method can be called only once per
     * call to next.
     *
     * @throws UnsupportedOperationException if the remove operation is not supported by this Iteration.
     * @throws IllegalStateException         If the Iteration has been closed, or if <tt>next()</tt> has not
     *                                       yet been called, or <tt>remove()</tt> has already been called
     *                                       after the last call to <tt>next()</tt>.
     */
    @Override
    public void remove() throws SailException {

    }

    private class QueuingListener extends ClientCall.Listener<T> {
        private boolean done = false;

        @Override
        public void onHeaders(Metadata headers) {
        }

        @Override
        public void onMessage(T value) {
            Preconditions.checkState(!done, "ClientCall already closed");
            buffer.add(value);
        }

        @Override
        public void onClose(Status status, Metadata trailers) {
            Preconditions.checkState(!done, "ClientCall already closed");
            if (status.isOk()) {
                buffer.add(ClosableResponseStream.this);
            } else {
                buffer.add(status);
            }
            done = true;
        }

    }
}
