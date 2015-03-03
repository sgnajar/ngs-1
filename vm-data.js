"use strict";

var tty = require('tty');

function assert_data(data) {
		if(Object.prototype.toString.call(data) !== '[object Array]') {
			console.log('Got non-data', data);
			throw new Error('Got non-data: ' + Object.toString(data));
		}
}

function make_getter(type, getter_name, processor) {
	var r = function(data) {
		assert_data(data);
		if(data[0] !== type) {
			console.log('Got data of unexpected type. Expected', type, 'got', data[0]);
			throw new Error('Got non-'+type+': ' + Object.toString(data));
		}
		if(processor) {
			data[1] = processor(data[1]);
		}
		return data[1];
	}
	exports['get_' + getter_name] = r;
}

function get_type(data) {
	assert_data(data);
	return data[0];
}

function get_meta(data) {
	assert_data(data);
	if(!data[2]) {
		data[2] = {}
	}
	return data[2];
}

make_getter('Array',		'arr');
make_getter('Bool',			'boo');
make_getter('Code',			'cod');
make_getter('Hash',			'hsh');
make_getter('Lambda',		'lmb');
make_getter('NativeMethod', 'nm');
make_getter('Null',			'nul');
make_getter('Number',		'num');
make_getter('Process',		'prc');
make_getter('Readline',		'rl');
make_getter('Scopes',		'scp');
make_getter('String',		'str');
make_getter('Thread',		'thr');

make_getter('Stream', 'stm', function(s) {
	if(s != 'stdin' && s != 'stdout' && s != 'stderr') {
		throw new Error('Currently supported streams are only stdin, stdout and stderr');
	}
	return process[s];
});

exports.get_type = get_type;
exports.get_meta = get_meta;
